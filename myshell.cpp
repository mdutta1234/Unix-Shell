#include <bits/stdc++.h>
#include <filesystem>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <glob.h>
#include <readline/readline.h>
#include <readline/history.h>

namespace fs = std::filesystem;

class Shell {
public:
    Shell() { 
        // Load persistent history for readline
        read_history(history_file.c_str());
        // Sync readline history to manual history vector
        HIST_ENTRY **entries = history_list();
        if (entries) {
            for (int i = 0; entries[i]; ++i)
                history_vec.push_back(entries[i]->line);
        }
    }
    ~Shell() {
        // Write readline history back to file
        write_history(history_file.c_str());
    }
    int run();

private:
    std::vector<std::string> history_vec;
    const std::string history_file = std::string(getenv("HOME")) + "/.myshell_history";

    void prompt();
    std::string read_line();
    std::vector<std::string> split(const std::string &line);
    std::vector<std::string> expand_wildcards(const std::vector<std::string>& args);
    int execute(const std::vector<std::string>& args);
    int launch(const std::vector<std::string>& args, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO);

    // Builtins
    int cmd_cd(const std::vector<std::string>&);
    int cmd_help(const std::vector<std::string>&);
    int cmd_history(const std::vector<std::string>&);
    int cmd_issue(const std::vector<std::string>&);
    int cmd_ls(const std::vector<std::string>&);
    int cmd_rm(const std::vector<std::string>&);
    int cmd_rmexcept(const std::vector<std::string>&);
};

int Shell::run() {
    while (true) {
        prompt();
        auto line = read_line();
        if (line.empty()) 
        	continue;
        history_vec.push_back(line);
        auto args = split(line);
        if (execute(args) == 0) 
        	break;
    }
    return 0;
}

void Shell::prompt() {
    // readline prompt won't include newline
    std::cout.flush();
}

std::string Shell::read_line() {
    // Build prompt string
    std::string cwd = fs::current_path().string();
    std::string prompt_str = std::string(getenv("USER")) + "@" + cwd + " $ ";
    char *input = readline(prompt_str.c_str());
    if (!input) 
    	return "";  // EOF
    std::string line(input);
    free(input);
    if (!line.empty()) {
        add_history(line.c_str());
        append_history(1, history_file.c_str());
    }
    return line;
}

std::vector<std::string> Shell::split(const std::string &line) {
    std::istringstream iss(line);
    return { std::istream_iterator<std::string>(iss), {} };
}

std::vector<std::string> Shell::expand_wildcards(const std::vector<std::string>& args) {
    std::vector<std::string> result;
    for (const auto& arg : args) {
        glob_t glob_result;
        if (glob(arg.c_str(), GLOB_TILDE, nullptr, &glob_result) == 0) {
            for (size_t i = 0; i < glob_result.gl_pathc; ++i)
                result.push_back(glob_result.gl_pathv[i]);
                globfree(&glob_result);
        } 
        else {
            result.push_back(arg);
        }
    }
    return result;
}

int Shell::execute(const std::vector<std::string>& raw_args) {
    if (raw_args.empty()) return 1;
    auto args = expand_wildcards(raw_args);
    // Handle piping
    std::vector<std::vector<std::string>> cmds;
    
    std::vector<std::string> current;
    for (auto &tok : args) {
        if (tok == "|") { 
        cmds.push_back(current); 
        current.clear(); 
        }
        else
         current.push_back(tok);
    }
    cmds.push_back(current);

    if (cmds.size() == 1) {
        auto cmd = cmds[0];
        if (cmd.empty()) return 1;
        // Builtins
        if (cmd[0] == "cd") 
        	return cmd_cd(cmd);
        	
        if (cmd[0] == "help") 
        	return cmd_help(cmd);
        	
        if (cmd[0] == "history") 
        	return cmd_history(cmd);

        if (cmd[0] == "issue") 
        	return cmd_issue(cmd);

        if (cmd[0] == "ls") 
        	return cmd_ls(cmd);

        if (cmd[0] == "rm") 
        	return cmd_rm(cmd);

        if (cmd[0] == "rmexcept") 
        	return cmd_rmexcept(cmd);

        if (cmd[0] == "exit") 
        	return 0;
        // I/O redirection
        int in_fd = STDIN_FILENO, out_fd = STDOUT_FILENO;
        std::vector<std::string> clean;
        for (size_t i = 0; i < cmd.size(); ++i) {

            if (cmd[i] == "<" && i+1 < cmd.size()) {
            	 in_fd = open(cmd[i+1].c_str(), O_RDONLY); i++; 
            	 }
            else if (cmd[i] == ">" && i+1 < cmd.size()) { 
            	out_fd = open(cmd[i+1].c_str(), O_WRONLY|O_CREAT|O_TRUNC,0644); i++; 
            }
            else if (cmd[i] == ">>" && i+1 < cmd.size()) { 
            	out_fd = open(cmd[i+1].c_str(), O_WRONLY|O_CREAT|O_APPEND,0644); i++; 
            }
            else
            	clean.push_back(cmd[i]);

        }
        return launch(clean, in_fd, out_fd);
    }
    // Multiple commands (piping)
    int n = cmds.size(); std::vector<int> pfd(2);
    int input_fd = STDIN_FILENO;
    for (int i = 0; i < n; ++i) {
        if (i < n-1)
        	pipe(pfd.data());

        launch(cmds[i], input_fd, (i<n-1? pfd[1] : STDOUT_FILENO));
        
        if (i<n-1) 
        	close(pfd[1]);
        
        if (input_fd!=STDIN_FILENO) 
        	close(input_fd);
        
        input_fd = (i<n-1? pfd[0] : STDIN_FILENO);
    }
    if (input_fd!=STDIN_FILENO) 
        close(input_fd);
    while (wait(nullptr)>0);
    
    tcsetpgrp(STDIN_FILENO, getpid());
    return 1;
}

int Shell::launch(const std::vector<std::string>& args, int in_fd, int out_fd) {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child — restore default signal handlers
        signal(SIGTSTP, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        // Put the child in its own process group
        setpgid(0, 0);
        // Give terminal control to the child
        tcsetpgrp(STDIN_FILENO, getpid());

        // I/O redirection...
        if (in_fd != STDIN_FILENO) { 
        	dup2(in_fd, STDIN_FILENO); 
        	close(in_fd); 
        }
        if (out_fd != STDOUT_FILENO) { 
        	dup2(out_fd, STDOUT_FILENO); 
        	close(out_fd); 
        }

        // exec
        std::vector<char*> cargv;
        
        for (auto &s : args) 
        	cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        perror("exec");
        exit(EXIT_FAILURE);
    }
    else if (pid < 0) {
        perror("fork");
        return 1;
    }
    else {
        // Parent — make sure child is in its own group
        setpgid(pid, pid);
        // Give terminal to the child while it runs
        tcsetpgrp(STDIN_FILENO, pid);

        int status;
        waitpid(pid, &status, WUNTRACED);

        // If child was stopped (SIGTSTP), print a notice
        if (WIFSTOPPED(status)) {
            std::cout << "[" << pid << "] Suspended\n";
        }

        // Return terminal control to the shell
        tcsetpgrp(STDIN_FILENO, getpid());
    }
    return 1;
}


// Builtins
int Shell::cmd_cd(const std::vector<std::string>& args) {
    if (args.size()<2) { 
    	std::cerr<<"cd: missing argument\n"; 
    	return 1; 
    }
    if (chdir(args[1].c_str())!=0) 
    	perror("cd");
    return 1;
}
int Shell::cmd_help(const std::vector<std::string>&){
    std::cout<<"Built-in commands:\n  cd help history issue ls rm rmexcept exit\n";
    return 1;
}
int Shell::cmd_history(const std::vector<std::string>&){
    for (size_t i=0;i<history_vec.size();++i)
        std::cout<<i+1<<"  "<<history_vec[i]<<"\n";
    return 1;
}
int Shell::cmd_issue(const std::vector<std::string>& args){
    if(args.size()<2) 
    	return cmd_history(args);
    	
    int idx=std::stoi(args[1]);
    if(idx<1||idx>(int)history_vec.size()) 
    	return 1;
    
    auto tokens=split(history_vec[idx-1]);
    
    return execute(tokens);
}
int Shell::cmd_ls(const std::vector<std::string>&){ 
	for(auto &p:fs::directory_iterator(".")) {
		std::cout<<p.path().filename()<<"\n";
	} 
		return 1; 
}
int Shell::cmd_rm(const std::vector<std::string>& args){ bool rec=false,verb=false; std::string tgt;
    for(size_t i=1;i<args.size();++i){ 
    	if(args[i]=="-r") 
    		rec=true; 
    	else if(args[i]=="-v") 
    		verb=true; 
    	else 
    		tgt=args[i]; 
    	}
    if(tgt.empty()) 
    	return 1;
    if(rec) 
    	fs::remove_all(tgt); 
    else
   	fs::remove(tgt);
    if(verb)
       std::cout<<tgt<<"\n";
    return 1;
}
int Shell::cmd_rmexcept(const std::vector<std::string>& args){ 
	std::set<std::string> keep(args.begin()+1,args.end());
    for(auto&p:fs::directory_iterator(".")){
        auto n=p.path().filename().string();
        if(n=="."||n=="..") 
         continue;
        if(!keep.count(n)){
         fs::remove_all(p.path());
          std::cout<<n<<"\n";
        }
    }
    return 1;
}

int main() {
    // Prevent the shell itself from ever being stopped by background-terminal writes:
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);  // you already had this for CTRL+Z

    // your existing SIGINT handler…
    signal(SIGINT, [](int){ std::cout << "\n"; });

    Shell shell;
    return shell.run();
}



