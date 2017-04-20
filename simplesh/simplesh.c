#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <memory>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <algorithm>

void check(int statement ,char *message) {
	if(statement == -1) {
		perror(message);
		exit(EXIT_FAILURE);
	}
}

std::string reminder = "";
const unsigned int BUFFER_SIZE = 1024;

std::string read_command() {
	std::vector<char> buffer(BUFFER_SIZE, 0);
	std::string result{reminder};

	int len;
	while((len = read(STDIN_FILENO, buffer.data(), BUFFER_SIZE - 1)) > 0) {
		auto pos = std::find(buffer.begin(), buffer.begin() + len, '\n');
		if( pos == buffer.begin() + len) {
			result.append(buffer.data(), len);
			reminder = "";
		} else {
			int offset = pos - buffer.begin();
			result.append(buffer.data(), std::min(offset, len));
			pos++;
			reminder = {pos, buffer.begin() + len};
			return result;
		}
	}

	if(len < 0) {
		perror("read error");
	}

	return result;

}

static int MAX_COMMAND_LEN = 100;
static char COMMAND_DELIM = '|';
static char SPACE = ' ';

std::vector<std::string> split_by(const std::string &input, char delim) {
	size_t len = input.length();
	size_t cur_len = 0;
	std::vector<std::string> result;

	for(size_t i = 0; i < len; i++) {
		if(input[i] != delim) {
			cur_len++;
		} else if(cur_len > 0){
			result.push_back(input.substr(i - cur_len, cur_len));
			cur_len = 0;
		}
	}
	if(cur_len > 0) {
		result.push_back(input.substr(len - cur_len, cur_len));
	}

	return result;
}

struct execargs_t {
	char **arguments;
	execargs_t(std::vector<std::string> &args) {
		arguments = (char **) malloc(sizeof(char*) * (args.size() + 1));
		for(size_t i = 0; i < args.size(); i++) {
			arguments[i] = const_cast<char*>(args[i].c_str());
		}
		arguments[args.size()] = NULL;
	}

	~execargs_t() {
		free(arguments);
	}
};

std::shared_ptr<execargs_t> make_args_struct(std::vector<std::string> values) {
	return std::make_shared<execargs_t>(values);
}

int exec(std::shared_ptr<execargs_t> execargs) {
	pid_t child = fork();
	if(child == -1) {
		perror("fork");
		return -1;
	}
	if(child == 0) {
		if(execvp(execargs->arguments[0], execargs->arguments) == -1) {
			perror((execargs->arguments)[0]);
			exit(1);
		}
		exit(0);
	}
	return child;
}

static int * children;
static int count = -1;
static int stdin_def = -1;
static int stdout_def = -1;
static struct sigaction old;

static void sigint_handler(int sig, siginfo_t *siginfo, void *context) {
	if(count != -1) {
		// stop workers
		for(int i = 0; i < count; i++) {
			if(children[i] != -1) {
				kill(children[i], SIGKILL);
				waitpid(children[i], 0, 0);
				children[i] = -1;
			}
		}

		if(stdin_def != -1) {
			check(dup2(stdin_def, STDIN_FILENO), "dup28");
			check(close(stdin_def), "close");
			stdin_def = -1;
		}
		if(stdout_def != -1) {
			check(dup2(stdout_def, STDOUT_FILENO), "dup29");
			check(close(stdout_def), "close");
			stdout_def = -1;
		}

		count = -1;
	}
}

int runpiped(const std::vector<std::shared_ptr<execargs_t> > &commands, size_t n) {
	int pipefd[2];

	stdin_def = dup(STDIN_FILENO);
	check(stdin_def, "dup");
	stdout_def = dup(STDOUT_FILENO);
	check(stdout_def, "dup");

	int a[n];
	for(size_t i = 0; i < n; i++) {
		a[i] = -1;
	}
	children = a;
	count = n;

	for(int i = 0; i < count; i++) {
		if(i != n - 1) {
			check(pipe(pipefd), "pipe"); // 1 write end, 0 read end
			check(dup2(pipefd[1], STDOUT_FILENO), "dup21");
			check(close(pipefd[1]), "close");
		} else {
			check(dup2(stdout_def, STDOUT_FILENO), "dup22");
		}
		children[i] = exec(commands[i]);
		if(i != n - 1) {
			check(dup2(pipefd[0], STDIN_FILENO), "dup23");
			check(close(pipefd[0]), "dup24");
		} else {
			check(close(STDIN_FILENO), "close");
		}
	}

	for(int i = 0; i < count; i++) {
		int status;
		waitpid(children[i], &status, 0);
		if(status != 0) {
			return status;
		}
	}

	check(dup2(stdin_def, STDIN_FILENO), "dup25");
	check(close(stdin_def), "close");
	check(close(stdout_def), "close");
	stdin_def = stdout_def = -1;
	return 0;
}
 
int main(int argc, char * argv[]) {
	struct sigaction act;
	memset(&act, '\0', sizeof(act));
	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = &sigint_handler;
	if(sigaction(SIGINT, &act, &old) < 0) {
		perror("Sigaction error");
		return 1;
	}

	char * 	symbol = (char *)"$ ";
	int offset = 0;
	char buffef[100];
	while(1) {
		int w = write(STDOUT_FILENO, symbol, strlen(symbol));		
		
		std::string input = read_command();
		if(input == "") {
			break;
		}
		if(input == "exit" || input == "quit") {
			exit(1);
		}
		std::vector<std::string> commands = split_by(input, COMMAND_DELIM);
		std::vector<std::shared_ptr<execargs_t> > execs;
		char * s;
		for(size_t i = 0; i < commands.size(); i++) {
			std::vector<std::string> parts = split_by(commands[i], SPACE);
			std::shared_ptr<execargs_t> current = make_args_struct(parts);
			execs.push_back(current);
			s = execs[i]->arguments[0];
		}
		int exit_code = runpiped(execs, execs.size());
	}

	return 0;
}