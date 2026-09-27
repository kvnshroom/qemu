/* Minimal PID 1 for the sun3/sun3x test initramfs (nolibc, static). */
#include <sys/mount.h>
int main(void)
{
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	printf("===== Linux/sun3 userspace is up (PID 1 running from initramfs) =====\n");
	printf("Starting an interactive smolsh shell on /dev/console\n");
	for (;;) {
		int pid = fork();
		if (pid == 0) {
			char *argv[] = { "/bin/smolsh", NULL };
			execve("/bin/smolsh", argv, NULL);
			printf("init: exec /bin/smolsh failed: %d\n", errno);
			return 1;
		}
		if (pid < 0) {
			printf("init: fork failed: %d\n", errno);
			return 1;
		}
		{
			int st = 0;
			waitpid(pid, &st, 0);
			if (WIFSIGNALED(st))
				printf("init: shell killed by signal %d; respawning\n", WTERMSIG(st));
			else
				printf("init: shell exited with status %d; respawning\n", WEXITSTATUS(st));
		}
		sleep(1);
	}
}
