#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* 두 스레드가 공유하는 장치 파일 디스크립터와 로그 파일. */
static int fd;
static FILE *logfile;
/* 블로킹 read로 이벤트를 받고 화면과 로그 파일에 기록한다. */
static void *logger(void *arg){
	char buf[128];
	ssize_t n;
	int old;
	(void)arg;
	while (1) {
		n = read(fd, buf, sizeof(buf)-1); /* cancellation point */
		if (n < 0) {
			if (errno == EINTR) continue;
			perror("read"); break;
		}
		if (n == 0) break;
		buf[n] = '\0';
		/* 파일 출력 도중에는 취소를 막아 로그를 안전하게 기록한다. */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
		fputs(buf, stdout); fflush(stdout);
		fputs(buf, logfile); fflush(logfile);
		pthread_setcancelstate(old, NULL);
	}
	return NULL;
}

/* 표준 입력을 받아 커널 명령으로 변환하고 write로 전달한다. */
static void *commands(void *arg){
	char *line = NULL;
	size_t cap = 0;
	const char *cmd;
	ssize_t n;
	(void)arg;
	while (1) {
		n = getline(&line, &cap, stdin);
		if (n < 0) break;
		line[strcspn(line, "\r\n")] = '\0';
		if (!strcmp(line, "quit")) break;
		if (!strcmp(line, "arm")) cmd = "ARM";
		else if (!strcmp(line, "disarm")) cmd = "DISARM";
		else if (!strcmp(line, "panic")) cmd = "PANIC";
		else { fprintf(stderr, "commands: arm, disarm, panic, quit\n"); continue; }
		if (write(fd, cmd, strlen(cmd)) < 0) perror("write");
	}
	free(line);
	return NULL;
}

/* 장치와 로그 파일을 열고 두 스레드를 실행한 뒤 안전하게 종료한다. */
int main(void){
	pthread_t log_thread, cmd_thread;
	int rc, result = EXIT_FAILURE;
	fd = open("/dev/guard", O_RDWR);
	if (fd < 0) { perror("/dev/guard"); return EXIT_FAILURE; }
	logfile = fopen("guard.log", "a");
	if (!logfile) { perror("guard.log"); close(fd); return EXIT_FAILURE; }
	rc = pthread_create(&log_thread, NULL, logger, NULL);
	if (rc) { fprintf(stderr, "logger: %s\n", strerror(rc)); goto out; }
	rc = pthread_create(&cmd_thread, NULL, commands, NULL);
	if (rc) fprintf(stderr, "commands: %s\n", strerror(rc));
	else {
		pthread_join(cmd_thread, NULL);
		result = EXIT_SUCCESS;
	}
	/* quit 또는 EOF 이후 read에서 잠든 로거도 취소하고 종료를 기다린다. */
	pthread_cancel(log_thread);
	pthread_join(log_thread, NULL);
out:
	fclose(logfile);
	close(fd);
	return result;
}
