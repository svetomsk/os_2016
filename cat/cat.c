#include <unistd.h>

#define BUF_SIZE 1024

void report_error(char * msg) {
    perror(msg);
}

int main(int argc, char ** argv) {
    char buf[BUF_SIZE];

    while(1) {
        ssize_t r = read(STDIN_FILENO, buf, BUF_SIZE);
        if(r == 0) {
            break;
        }
        if(r == -1) {
            report_error("error while reading");
            break;
        }
        ssize_t current = 0;
        while(current < r) {
            ssize_t w = write(STDOUT_FILENO, buf + current, r - current);
            if(w == -1) {
                report_error("error while writing");
                break;
            }
            current += w;
        }
    }
    return 0;
}
