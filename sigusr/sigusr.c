#include <unistd.h>
#include <signal.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

static void sigusr1_handler(int sig, siginfo_t *siginfo, void *context) {
	printf("Got SIGURS1 from PID: %ld\n", (long)siginfo->si_pid);
	exit(1);
}

static void sigusr2_handler(int sig, siginfo_t *siginfo, void *context) {
	printf("Got SIGURS2 from PID: %ld\n", (long)siginfo->si_pid);
	exit(1);
}

int main(int argc, char *argv[]) {
	struct sigaction act;

	memset(&act, '\0', sizeof(act));

	act.sa_flags = SA_SIGINFO;


	act.sa_sigaction = &sigusr1_handler;
	if(sigaction(SIGUSR1, &act, NULL) < 0) {
		perror("sigusr1 error");
		return 1;
	}
	act.sa_sigaction = &sigusr2_handler;
	if(sigaction(SIGUSR2, &act, NULL) < 0) {
		perror("sigusr2 error");
		return 1;
	}

	sleep(10);
	printf("%s\n", "No signals were caught");
	return 0;
}