#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <termios.h>
#include <vector>
#include <map>
#include <sys/ioctl.h>
#include <deque>
#include <fstream>
#include <signal.h>

const int BUFFER_SIZE = 2048;

struct communicator {
	int self;
	std::shared_ptr<communicator> other;
	pid_t child = -1;

	char read_buf[BUFFER_SIZE];
	std::deque<std::string> write_buf; 

	communicator(int fd) : self(fd) {
		memset(read_buf, 0, BUFFER_SIZE);
	}

	int read_() {
		std::string res;
		ssize_t count;

		while((count = read(self, read_buf, sizeof read_buf)) > 0) {
			if(count == -1) {
				if(errno != EAGAIN) {
					perror("read");
					return -1;
				} // else no more data to read
				break;
			} else if(count == 0) { // connection closed. EOF
				break;
			}
			res += std::string(read_buf, count);
		}
		if(res.size() == 0) return -1;
		other->write_buf.push_back(res);
		other->write_();
		return 0;
	}

	int write_() {
		while(!write_buf.empty()) {
			std::string str = write_buf.front();
			write_buf.pop_front();
			const char* cur_buf = str.c_str();
			size_t len = str.size();
			ssize_t written = 0;
			ssize_t res;
			while(written < len && (res = write(self, cur_buf + written, len - written))) {
				if(res == -1) {
					break;
				}
				written += res;
			}
			if(written < len) {
				if(errno != EAGAIN) {
					return -1;
				}
				std::string unwritten(cur_buf + written, len - written);
				write_buf.push_front(unwritten);
				break;
			}
		}
		return 0;
	}

	void close_sock() {
		if(close(self) == -1) {
			perror("close");
		}
	}
};

typedef std::shared_ptr<communicator> communicator_ptr;

static std::vector<communicator_ptr> clients;
static std::vector<communicator_ptr> terms;

/**
  * Creates and binds socket
**/
int create_and_bind(const char *port) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int s, sfd;

	memset(&hints, 0, sizeof (struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	s = getaddrinfo(NULL, port, &hints, &result);
	if (s != 0) {
		perror("getaddrinfo");
		return -1;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1) {
			continue;
		}

		s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
		if (s == 0) {
			break;
		}
		close(sfd);
	}

	if (rp == NULL) {
		perror("Could not bind");
		return -1;
	}

	freeaddrinfo(result);
	return sfd;
}

/**
  * Makes socket non-blocking
**/
int make_socket_non_blocking(int sfd) {
	int flags, s;

	flags = fcntl(sfd, F_GETFL, 0);
	if (flags == -1) {
		perror("fcntl");
		return -1;
	}

	flags |= O_NONBLOCK;
	s = fcntl(sfd, F_SETFL, flags);
	if(s == -1) {
		perror("fcntl");
		return -1;
	}

	return 0;
}

int create_pty() {
	int fdm = posix_openpt(O_RDWR);
	if(fdm < 0) {
		perror("posix_openpt");
		return -1;
	}

	if(grantpt(fdm) || unlockpt(fdm)) {
		perror("grantpt or unlockpt");
		return -1;
	}
	return fdm;
}

int add_to_epoll(int efd, communicator_ptr &lookfd) {
	epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET;
	ev.data.ptr = (void*)(lookfd.get());
	printf("Adding fd %d\n", lookfd->self);
	if(epoll_ctl(efd, EPOLL_CTL_ADD, lookfd->self, &ev) == -1) {
		perror("epoll_ctl");
		return -1;
	}
	return 0;
}

/**
  * Accepts all pending incoming connections
**/
void handle_incoming_connections(int sfd, int efd) {
	int s;
	while(1) {
		struct sockaddr in_addr;
		socklen_t in_len;
		int infd;
		char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

		in_len = sizeof in_addr;
		infd = accept(sfd, &in_addr, &in_len);
		if(infd == -1) {
			if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				// no more incoming connections
				break;
			} else {
				perror("accept");
				break;
			}
		}
		communicator_ptr client = std::make_shared<communicator>(infd);

		s = getnameinfo(&in_addr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf,
						NI_NUMERICHOST | NI_NUMERICSERV);
		if(s == 0) {
			printf("Accepted connection on descriptor %d (host=%s ,port=%s)\n", client->self, hbuf, sbuf);
			clients.push_back(client);
			s = make_socket_non_blocking(client->self);
			if(s == -1) {
				abort();
			}

			s = add_to_epoll(efd, client);
			if(s == -1) {
				perror("add_to_epoll");
				abort();
			}

			int terminal = create_pty();
			if(terminal == -1) {
				perror("create_pty");
				break;
			}

			communicator_ptr terminal_com = std::make_shared<communicator>(terminal);
			terminal_com->other = client;
			client->other = terminal_com;

			terms.push_back(terminal_com);
			s = make_socket_non_blocking(terminal);
			if(s == -1) {
				abort();
			}
			s = add_to_epoll(efd, terminal_com);
			if(s == -1) {
				abort();
			}

			int slave = open(ptsname(terminal), O_RDWR);
			pid_t pid = fork();
			if(pid == -1) {
				perror("fork");
				abort();
			}
			if(pid == 0) { // child
				// close all unused descriptors
				for(communicator_ptr fd : clients) {
					fd->close_sock();
				}
				for(communicator_ptr fd : terms) {
					fd->close_sock();
				}
				close(infd);
				close(terminal);
				close(sfd);
				close(efd);

				struct termios original_settings;
				struct termios new_settings;
				tcgetattr(slave, &original_settings);
				new_settings = original_settings;
				new_settings.c_lflag &= ~(ECHO | ECHONL | ICANON);

				tcsetattr(slave, TCSANOW, &new_settings);

				dup2(slave, STDIN_FILENO);
				dup2(slave, STDOUT_FILENO);
				dup2(slave, STDERR_FILENO);
				close(slave);

				setsid();

				ioctl(0, TIOCSCTTY, 1);

				execlp("/bin/sh", "sh", NULL);
			} else {
				close(slave);
				client->child = pid;
			}
		}		
	}
}

/**
  * Reads incoming message
**/
void handle_messages(int sfd, int efd, epoll_event * events, int i) {
	int s = -1;
	communicator* comm = (communicator*)events[i].data.ptr;
	if((events[i].events & EPOLLIN) != 0) { // have something to read
		s = comm->read_();
	} else if((events[i].events & EPOLLOUT) != 0) { // have something to write
		s = comm->write_();
	}
	if(s == -1) {
		communicator_ptr pair = comm->other;
		for(auto it = clients.begin(); it != clients.end(); it++) {
			if(it->get() == comm) {
				epoll_ctl(efd, EPOLL_CTL_DEL, it->get()->self, events + i);
				clients.erase(it);
				break;
			}
		}
		auto other = comm->other;
		for(auto it = terms.begin(); it != terms.end(); it++) {
			if(it->get() == other.get()) {
				epoll_ctl(efd, EPOLL_CTL_DEL, it->get()->self, events + i);
				terms.erase(it);
				break;
			}
		}
	}

}

void demonize() {
	std::ifstream in("/tmp/rshd.pid");
	if(in) {
		pid_t pid;
		in >> pid;
		if(!kill(pid, 0)) {
			perror("Daemon is already running");
			exit(pid);
		}
	}
	in.close();

	pid_t res = fork();
	if(res == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if(res != 0) {
		exit(EXIT_SUCCESS);
	}

	setsid();
	pid_t daemon_pid = fork();
	if(daemon_pid) {
		std::ofstream out("/tmp/rshd.pid");
		out << daemon_pid;
		out.close();
		exit(EXIT_SUCCESS);
	}
	int slave = open("/dev/null", O_RDWR);
	int err = open("/tmp/rshd.err.log", O_CREAT | O_TRUNC | O_WRONLY, 0644);
	dup2(slave, STDIN_FILENO);
	dup2(slave, STDOUT_FILENO);
	dup2(err, STDERR_FILENO);
	close(slave);
	close(err);

	return;

}

const int MAX_EVENTS = 64;

int main(int argc, char **argv) {

	demonize();
	int sfd, s;
	int efd;
	struct epoll_event event;
	struct epoll_event *events;

	std::string port = "5053";
	sfd = create_and_bind(port.c_str());
	if( sfd == -1) {
		abort();
	}

	s = make_socket_non_blocking(sfd);
	if(s == -1) {
		abort();
	}

	s = listen(sfd, SOMAXCONN);
	if(s == -1) {
		perror("listen");
		abort();
	}

	efd = epoll_create1(0);
	if(efd == -1) {
		perror("epoll_create1");
		abort();
	}
	auto sfd_ptr = std::make_shared<communicator>(sfd);
	epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = (void*)(sfd_ptr.get());
	if(epoll_ctl(efd, EPOLL_CTL_ADD, sfd_ptr->self, &ev)) {
		perror("epoll_ctl: listen socket");
		close(efd);
		exit(EXIT_FAILURE);
	}

	add_to_epoll(efd, sfd_ptr);

	events = (epoll_event *)calloc(MAX_EVENTS, sizeof event);

	while(1) {
		int n, i;
		n = epoll_wait(efd, events, MAX_EVENTS, -1);
		for(i = 0; i < n; i++) {
			communicator* comm = (communicator*) events[i].data.ptr;
			if(sfd == comm->self) { // incoming connection 1 or more
				handle_incoming_connections(sfd, efd);
				continue;
			} else { // got some data to read
				handle_messages(sfd, efd, events, i);
			}
		}
	}

	free(events);
	close(sfd);
	return EXIT_SUCCESS;
}