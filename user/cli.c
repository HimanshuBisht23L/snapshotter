// ==== mainCode/user/cli.c ====
// small fixes applied (typo removal, cleaned includes, minor robustness)
// Compile: gcc -O2 -Wall -o cli cli.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/ioctl.h>

/* constants */
#define MAX_SAVED 64
#define NAME_LEN 512
#define DEVICE "/dev/snapshotctl"

#define IOCTL_SNAPSHOT _IOW('s', 1, int)

struct snap_ioc
{
	pid_t oldpid;
	pid_t newpid;
};
#define IOCTL_RESTORE _IOW('s', 2, struct snap_ioc)

typedef struct
{
	pid_t pid;
	char name[NAME_LEN];
	int is_gui;
} Process;

typedef struct
{
	pid_t old_pid;
	char name[NAME_LEN];
	char exe_path[NAME_LEN];
	char *cmdline;			 // malloc'd buffer with '\0' separated argv
	char tty_path[NAME_LEN]; /* e.g. /dev/pts/3 */
} SavedProcess;

SavedProcess saved[MAX_SAVED];
int saved_count = 0;

/* helpers */
int is_number(const char *s)
{
	if (!s) return 0;
	while (*s)
	{
		if (!isdigit((unsigned char)*s))
			return 0;
		s++;
	}
	return 1;
}

int is_gui_process(pid_t pid)
{
	char path[NAME_LEN], buf[NAME_LEN];
	FILE *f;
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fgets(buf, sizeof(buf), f))
	{
		if (strstr(buf, "--type=renderer") || strstr(buf, "--type=gpu-process"))
		{
			fclose(f);
			return 0;
		}
	}
	fclose(f);

	snprintf(path, sizeof(path), "/proc/%d/environ", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	int gui = 0;
	while (fgets(buf, sizeof(buf), f))
	{
		if (strstr(buf, "DISPLAY=") || strstr(buf, "WAYLAND_DISPLAY="))
		{
			gui = 1;
			break;
		}
	}
	fclose(f);
	return gui;
}

int list_running(Process *list, int max)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int i = 0;
	if (!d)
		return 0;
	while ((e = readdir(d)) && i < max)
	{
		if (!is_number(e->d_name))
			continue;
		pid_t pid = atoi(e->d_name);
		char path[NAME_LEN], name[NAME_LEN];
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(name, sizeof(name), f))
		{
			fclose(f);
			continue;
		}
		name[strcspn(name, "\n")] = 0;
		fclose(f);
		list[i].pid = pid;
		strncpy(list[i].name, name, NAME_LEN - 1);
		list[i].name[NAME_LEN - 1] = '\0';
		list[i].is_gui = is_gui_process(pid);
		i++;
	}
	closedir(d);
	return i;
}

/* read /proc/<pid>/cmdline into a single buffer (null-separated) */
char *read_cmdline(pid_t pid)
{
	char path[NAME_LEN];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long sz = ftell(f);
	if (sz <= 0)
	{
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
	char *buf = malloc(sz + 1);
	if (!buf)
	{
		fclose(f);
		return NULL;
	}
	size_t r = fread(buf, 1, sz, f);
	fclose(f);
	buf[r] = '\0';
	return buf;
}

/* read /proc/<pid>/exe path */
int read_exe_path(pid_t pid, char *out, int outlen)
{
	char path[NAME_LEN];
	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	ssize_t r = readlink(path, out, outlen - 1);
	if (r < 0)
		return -1;
	out[r] = '\0';
	return 0;
}

/* parse cmdline buffer into argv array for execv (allocates argv array)
   returned argv points into cmdline buffer when available; otherwise new malloc'd strings */
char **cmdline_to_argv(char *cmdline)
{
	if (!cmdline)
		return NULL;
	int argc = 0;
	for (char *p = cmdline; *p;)
	{
		size_t len = strlen(p);
		argc++;
		p += len + 1;
	}
	char **argv = malloc((argc + 1) * sizeof(char *));
	if (!argv)
		return NULL;
	int i = 0;
	for (char *p = cmdline; *p;)
	{
		argv[i++] = p;
		p += strlen(p) + 1;
	}
	argv[i] = NULL;
	return argv;
}

/* utility: basename copy (returns malloc'd string) */
static char *dup_basename(const char *p)
{
	if (!p || !p[0])
		return NULL;
	char tmp[NAME_LEN];
	strncpy(tmp, p, NAME_LEN - 1);
	tmp[NAME_LEN - 1] = 0;
	char *b = basename(tmp);
	return strdup(b ? b : tmp);
}

/* helper to get basename pointer (non-destructive) */
static const char *path_basename_ptr(const char *p)
{
	if (!p || !p[0])
		return NULL;
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

/* Attempt to launch the saved program in a new terminal window */
static int launch_in_new_terminal(const SavedProcess *sp)
{
	static const char *term_list[] = {
		"terminator", "x-terminal-emulator", "gnome-terminal", "konsole",
		"xfce4-terminal", "lxterminal", "urxvt", "xterm", NULL};

	char cmdline_buf[4096] = {0};
	if (sp->cmdline)
	{
		char *p = sp->cmdline;
		int first = 1;
		while (p && *p)
		{
			if (!first)
				strncat(cmdline_buf, " ", sizeof(cmdline_buf) - strlen(cmdline_buf) - 1);
			strncat(cmdline_buf, p, sizeof(cmdline_buf) - strlen(cmdline_buf) - 1);
			p += strlen(p) + 1;
			first = 0;
		}
	}
	else if (sp->exe_path[0])
	{
		snprintf(cmdline_buf, sizeof(cmdline_buf), "%s", sp->exe_path);
	}
	else
	{
		snprintf(cmdline_buf, sizeof(cmdline_buf), "%s", sp->name[0] ? sp->name : "(unknown)");
	}

	/* wrapped to ensure shell exec semantics */
	char wrapped_cmd[8192];
	snprintf(wrapped_cmd, sizeof(wrapped_cmd), "sh -c 'exec %s'", cmdline_buf);

	for (const char **t = term_list; *t; ++t)
	{
		/* try to find terminal in PATH */
		if (access(*t, X_OK) == 0 || getenv("PATH"))
		{
			pid_t tf = fork();
			if (tf == 0)
			{
				/* child: attempt common exec variants */
				execlp(*t, *t, "-e", "sh", "-c", wrapped_cmd, (char *)NULL);
				execlp(*t, *t, "--", "sh", "-c", wrapped_cmd, (char *)NULL);
				execlp(*t, *t, "-x", wrapped_cmd, (char *)NULL);
				/* if fails, exit child */
				_exit(127);
			}
			else if (tf > 0)
			{
				/* parent: we launched a terminal */
				return 0;
			}
			else
			{
				continue;
			}
		}
	}

	/* fallback: best-effort system() */
	char syscmd[8192];
	snprintf(syscmd, sizeof(syscmd), "x-terminal-emulator -e sh -c '%s' &", cmdline_buf);
	system(syscmd);
	return 0;
}

/* Final spawn_from_saved(): attempts to reattach to saved TTY, otherwise
   launches the restored program in a quiet new terminal (terminator preferred),
   falls back to other emulators, then to nohup/ detached mode.
*/
pid_t spawn_from_saved(const SavedProcess *sp)
{
	if (!sp)
		return -1;

	pid_t child = fork();
	if (child < 0)
		return -1;

	if (child == 0)
	{
		/* ===== CHILD ===== */
		/* Unblock signals and restore default handlers so the exec'd program
		   receives signals normally. */
		sigset_t sset;
		sigemptyset(&sset);
		sigprocmask(SIG_SETMASK, &sset, NULL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTSTP, SIG_DFL);
		signal(SIGTTIN, SIG_DFL);
		signal(SIGTTOU, SIG_DFL);

		/* Build argv from saved cmdline if available */
		char **argv = NULL;
		if (sp->cmdline)
		{
			argv = cmdline_to_argv(sp->cmdline);
			if (!argv)
				_exit(127);
		}
		else
		{
			argv = malloc(2 * sizeof(char *));
			if (!argv)
				_exit(127);
			if (sp->exe_path[0])
				argv[0] = strdup(sp->exe_path);
			else if (sp->name[0])
				argv[0] = strdup(sp->name);
			else
				argv[0] = strdup("(unknown)");
			argv[1] = NULL;
		}

		/* Try to reattach to saved tty (best-effort). */
		int attached = 0;
		int tcres = -1;
		if (sp->tty_path[0])
		{
			/* Become session leader first */
			setsid(); /* ignore failure */

			/* Open slave PTY without O_NOCTTY so open may make it controlling tty */
			int ttyfd = open(sp->tty_path, O_RDWR);
			if (ttyfd >= 0)
			{
				/* Put ourselves in a fresh process group */
				setpgid(0, 0); /* ignore failure */

				/* Temporarily ignore SIGTTOU to avoid being stopped when changing FG */
				struct sigaction sa_old, sa_ignore;
				sa_ignore.sa_handler = SIG_IGN;
				sigemptyset(&sa_ignore.sa_mask);
				sa_ignore.sa_flags = 0;
				sigaction(SIGTTOU, &sa_ignore, &sa_old);

				pid_t mypgid = getpgrp();
				tcres = tcsetpgrp(ttyfd, mypgid);
				if (tcres == -1)
				{
					/* try ioctl fallback to become controlling tty, then try again */
					ioctl(ttyfd, TIOCSCTTY, 0);
					tcres = tcsetpgrp(ttyfd, mypgid);
				}

				/* restore SIGTTOU handler */
				sigaction(SIGTTOU, &sa_old, NULL);

				/* Redirect stdio to tty */
				dup2(ttyfd, STDIN_FILENO);
				dup2(ttyfd, STDOUT_FILENO);
				dup2(ttyfd, STDERR_FILENO);
				if (ttyfd > 2)
					close(ttyfd);

				attached = (tcres == 0);
			}
			else
			{
				/* log open failure */
				int lf = open("/tmp/snapshot_attach_log", O_WRONLY | O_CREAT | O_APPEND, 0644);
				if (lf >= 0)
				{
					dprintf(lf, "attach: pid=%d open(%s) failed errno=%d\n",
							getpid(), sp->tty_path, errno);
					close(lf);
				}
			}
		}

		/* Log attach attempt result */
		int attachfd = open("/tmp/snapshot_attach_log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (attachfd >= 0)
		{
			dprintf(attachfd,
					"attach: pid=%d exe='%s' tty='%s' attached=%d tcsetpgrp_res=%d errno=%d\n",
					getpid(),
					sp->exe_path[0] ? sp->exe_path : "(empty)",
					sp->tty_path[0] ? sp->tty_path : "(none)",
					attached, tcres, (tcres == -1 ? errno : 0));
			close(attachfd);
		}

		/* If not attached, try to launch in a new (quiet) terminal emulator. */
		if (!attached)
		{
			const char *exe = sp->exe_path[0] ? sp->exe_path : (argv && argv[0] ? argv[0] : "(unknown)");
			const char *terminal = getenv("TERM_PROGRAM"); /* optional hint */

			/* Terminator (priority) */
			if ((terminal && strstr(terminal, "terminator")) || access("/usr/bin/terminator", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "(exec terminator -x \"$1\" >/dev/null 2>&1) &",
					  "sh", exe, NULL);
			}

			/* GNOME Terminal */
			if ((terminal && strstr(terminal, "gnome")) || access("/usr/bin/gnome-terminal", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "(exec gnome-terminal -- \"$1\" >/dev/null 2>&1) &",
					  "sh", exe, NULL);
			}

			/* Konsole */
			if ((terminal && strstr(terminal, "konsole")) || access("/usr/bin/konsole", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "(exec konsole -e \"$1\" >/dev/null 2>&1) &",
					  "sh", exe, NULL);
			}

			/* XFCE4 Terminal */
			if ((terminal && strstr(terminal, "xfce4")) || access("/usr/bin/xfce4-terminal", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "(exec xfce4-terminal -e \"$1\" >/dev/null 2>&1) &",
					  "sh", exe, NULL);
			}

			/* XTerm */
			if (access("/usr/bin/xterm", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "(exec xterm -hold -e \"$1\" >/dev/null 2>&1) &",
					  "sh", exe, NULL);
			}

			/* Nohup/headless fallback (background to /tmp/restore.out) */
			if (access("/usr/bin/nohup", X_OK) == 0)
			{
				execl("/bin/sh", "sh", "-c",
					  "setsid nohup \"$1\" >/tmp/restore.out 2>&1 & disown",
					  "sh", exe, NULL);
			}

			/* Final fallback: detach to /dev/null */
			fprintf(stderr, "⚠️  No terminal emulator found; running detached.\n");
			setsid();
			int nullfd = open("/dev/null", O_RDWR);
			if (nullfd >= 0)
			{
				dup2(nullfd, STDIN_FILENO);
				dup2(nullfd, STDOUT_FILENO);
				dup2(nullfd, STDERR_FILENO);
				if (nullfd > 2)
					close(nullfd);
			}
		}

		/* Exec the restored program (last step) */
		if (sp->exe_path[0])
			execv(sp->exe_path, argv);
		else
			execvp(argv[0], argv);

		/* If exec fails, log error and exit child */
		{
			int e = errno;
			int ffd = open("/tmp/snapshot_exec_err", O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (ffd >= 0)
			{
				dprintf(ffd, "exec failed errno=%d (%s) exe='%s' argv0='%s' pid=%d\n",
						e, strerror(e),
						sp->exe_path[0] ? sp->exe_path : "(none)",
						argv[0] ? argv[0] : "(null)", getpid());
				close(ffd);
			}
		}

		_exit(127);
	}

	/* ===== PARENT ===== */
	return child;
}

/* remove saved entry with index idx */
void remove_saved_index(int idx)
{
	if (idx < 0 || idx >= saved_count)
		return;
	if (saved[idx].cmdline)
		free(saved[idx].cmdline);
	// shift remaining
	for (int i = idx; i < saved_count - 1; i++)
		saved[i] = saved[i + 1];
	saved_count--;
}

/* main */
int main(void)
{
	int fd = open(DEVICE, O_RDWR);
	if (fd < 0)
	{
		perror("open " DEVICE);
		fprintf(stderr, "Make sure kernel module is loaded and /dev/snapshotctl exists\n");
		return 1;
	}

	Process procs[1024];
	int running_count = 0;
	while (1)
	{
		printf("\nMenu:\n1. Snapshot & Kill (enter PID)\n2. Restore (enter old PID)\n3. Show Saved\n4. Exit\nChoice: ");
		int choice;
		if (scanf("%d", &choice) != 1)
		{
			while (getchar() != '\n')
				;
			continue;
		}
		while (getchar() != '\n')
			;
		if (choice == 1)
		{
			running_count = list_running(procs, 1024);
			if (running_count == 0)
			{
				printf("no processes found\n");
				continue;
			}
			printf("\n=== Running processes (showing name and PID) ===\n");
			for (int i = 0; i < running_count; i++)
				printf("PID: %d\tName: %s%s\n", procs[i].pid, procs[i].name, procs[i].is_gui ? " (GUI)" : "");

			printf("\nEnter PID to snapshot & kill: ");
			pid_t pid;
			if (scanf("%d", &pid) != 1)
			{
				while (getchar() != '\n')
					;
				continue;
			}
			while (getchar() != '\n')
				;

			// confirm PID exists in list
			int found = 0;
			for (int i = 0; i < running_count; i++)
				if (procs[i].pid == pid)
				{
					found = 1;
					break;
				}
			if (!found)
			{
				printf("PID %d not found in running list\n", pid);
				continue;
			}

			// read cmdline and exe path BEFORE killing
			char *cmdline = read_cmdline(pid);
			char exe_path[NAME_LEN] = {0};
			if (read_exe_path(pid, exe_path, sizeof(exe_path)) != 0)
				exe_path[0] = '\0';

			// Call kernel ioctl to record snapshot entry (kernel will hold ref)
			if (ioctl(fd, IOCTL_SNAPSHOT, pid) < 0)
			{
				perror("Snapshot ioctl failed");
				if (cmdline)
					free(cmdline);
				continue;
			}

			// store saved info in userland saved[] for restore
			if (saved_count < MAX_SAVED)
			{
				saved[saved_count].old_pid = pid;
				saved[saved_count].cmdline = cmdline; // may be NULL

				// find the chosen process name from procs[] (matching the PID we killed)
				for (int j = 0; j < running_count; j++)
				{
					if (procs[j].pid == pid)
					{
						strncpy(saved[saved_count].name, procs[j].name, NAME_LEN - 1);
						break;
					}
				}
				if (cmdline)
				{
					char *first = cmdline;
					strncpy(saved[saved_count].name, first, NAME_LEN - 1);
				}
				strncpy(saved[saved_count].exe_path, exe_path, NAME_LEN - 1);
				saved[saved_count].name[NAME_LEN - 1] = '\0';
				saved[saved_count].exe_path[NAME_LEN - 1] = '\0';

				/* save controlling terminal (fd0 or fd1) */
				saved[saved_count].tty_path[0] = '\0';
				{
					char fd0path[NAME_LEN];
					ssize_t r;
					snprintf(fd0path, sizeof(fd0path), "/proc/%d/fd/0", pid);
					r = readlink(fd0path, saved[saved_count].tty_path, sizeof(saved[saved_count].tty_path) - 1);
					if (r <= 0)
					{
						snprintf(fd0path, sizeof(fd0path), "/proc/%d/fd/1", pid);
						r = readlink(fd0path, saved[saved_count].tty_path, sizeof(saved[saved_count].tty_path) - 1);
					}
					if (r > 0)
						saved[saved_count].tty_path[r] = '\0';
					else
						saved[saved_count].tty_path[0] = '\0';
				}

				/* debug print */
				printf("DEBUG snapshot: pid=%d exe_path='%s' tty='%s' cmdline=%s\n",
					   pid,
					   saved[saved_count].exe_path[0] ? saved[saved_count].exe_path : "(none)",
					   saved[saved_count].tty_path[0] ? saved[saved_count].tty_path : "(none)",
					   cmdline ? cmdline : "(null)");

				saved_count++;
			}
			else
			{
				if (cmdline)
					free(cmdline);
				printf("Saved table full\n");
			}

			// kill process and its children (do this AFTER saving info)
			char cmd[256];
			snprintf(cmd, sizeof(cmd), "pkill -TERM -P %d; kill -9 %d", pid, pid);
			system(cmd);

			printf("Snapshot recorded and PID %d killed (process saved for restore)\n", pid);
		}
		else if (choice == 2)
		{
			if (saved_count == 0)
			{
				printf("No saved processes\n");
				continue;
			}
			printf("\nSaved processes:\n");
			for (int i = 0; i < saved_count; i++)
				printf("[%d] oldPID=%d name=%s exe=%s tty=%s\n", i + 1, saved[i].old_pid, saved[i].name,
					   saved[i].exe_path[0] ? saved[i].exe_path : "(no exe)",
					   saved[i].tty_path[0] ? saved[i].tty_path : "(no tty)");

			printf("\nEnter old PID to restore: ");
			pid_t oldpid;
			if (scanf("%d", &oldpid) != 1)
			{
				while (getchar() != '\n')
					;
				continue;
			}
			while (getchar() != '\n')
				;

			int idx = -1;
			for (int i = 0; i < saved_count; i++)
				if (saved[i].old_pid == oldpid)
				{
					idx = i;
					break;
				}
			if (idx < 0)
			{
				printf("Old PID %d not found\n", oldpid);
				continue;
			}

			printf("DEBUG restore: oldpid=%d saved.exe_path='%s' saved.cmdline=%s saved.tty='%s'\n",
				   saved[idx].old_pid,
				   saved[idx].exe_path[0] ? saved[idx].exe_path : "(empty)",
				   saved[idx].cmdline ? saved[idx].cmdline : "(null)",
				   saved[idx].tty_path[0] ? saved[idx].tty_path : "(none)");

			// spawn new process using saved metadata
			pid_t newpid = spawn_from_saved(&saved[idx]);
			if (newpid < 0)
			{
				perror("spawn failed");
				continue;
			}

			/* --- improved parent wait + immediate-exit capture --- */
			int status = 0;
			int child_exited = 0;
			pid_t wr = 0;

			/* poll waitpid for up to 500ms */
			for (int iter = 0; iter < 25; iter++)
			{ // 25 * 20ms = 500ms
				wr = waitpid(newpid, &status, WNOHANG);
				if (wr == -1)
				{
					perror("waitpid");
					break;
				}
				else if (wr == 0)
				{
					/* still running; wait a bit and retry */
					usleep(20000); /* 20ms */
					continue;
				}
				else if (wr == newpid)
				{
					/* child exited quickly */
					child_exited = 1;
					break;
				}
			}

			if (child_exited)
			{
				if (WIFEXITED(status))
				{
					printf("Child PID=%d exited with status %d\n", newpid, WEXITSTATUS(status));
				}
				else if (WIFSIGNALED(status))
				{
					printf("Child PID=%d killed by signal %d (%s)\n", newpid, WTERMSIG(status), strsignal(WTERMSIG(status)));
				}
				else
				{
					printf("Child PID=%d changed state (status=0x%x)\n", newpid, status);
				}

				/* show any logs the child wrote (spawn_log and exec_err) */
				printf("---- /tmp/snapshot_child_start_log ----\n");
				system("sed -n '1,200p' /tmp/snapshot_child_start_log 2>/dev/null || true");
				printf("---- /tmp/snapshot_spawn_log ----\n");
				system("sed -n '1,200p' /tmp/snapshot_spawn_log 2>/dev/null || true");
				printf("---- /tmp/snapshot_exec_err ----\n");
				system("sed -n '1,200p' /tmp/snapshot_exec_err 2>/dev/null || true");

				/* since child exited, treat spawn as failed and do not rebind */
				printf("Spawn failed (child exited). Will request kernel to release snapshot.\n");
				struct snap_ioc ioc;
				ioc.oldpid = oldpid;
				ioc.newpid = 0;
				if (ioctl(fd, IOCTL_RESTORE, &ioc) < 0)
					perror("Restore ioctl failed");
				else
					printf("Kernel released snapshot for oldpid=%d\n", oldpid);

				/* remove saved entry locally */
				remove_saved_index(idx);
				continue; /* go back to menu */
			}

			/* If child didn't exit, proceed with the earlier validation (reads /proc/<newpid>/exe etc.) */
			int alive = 0;
			/* wait briefly for child to appear (non-blocking already used above) */
			for (int t = 0; t < 20; t++)
			{
				if (kill(newpid, 0) == 0)
				{
					alive = 1;
					break;
				}
				if (errno == ESRCH)
				{
					usleep(20000);
					continue;
				}
				alive = 1;
				break;
			}

			if (alive)
			{
				/* stronger validation: read /proc/<newpid>/exe and /proc/<newpid>/comm */
				char exe_read[NAME_LEN] = {0};
				char comm_read[NAME_LEN] = {0};
				char procexe[64];
				char proccomm[64];
				snprintf(procexe, sizeof(procexe), "/proc/%d/exe", newpid);
				snprintf(proccomm, sizeof(proccomm), "/proc/%d/comm", newpid);

				ssize_t r = readlink(procexe, exe_read, sizeof(exe_read) - 1);
				if (r > 0)
					exe_read[r] = '\0';

				FILE *f = fopen(proccomm, "r");
				if (f)
				{
					if (fgets(comm_read, sizeof(comm_read), f))
					{
						comm_read[strcspn(comm_read, "\n")] = 0;
					}
					fclose(f);
				}

				int valid = 0;
				if (saved[idx].exe_path[0] && exe_read[0])
				{
					if (strcmp(saved[idx].exe_path, exe_read) == 0)
						valid = 1;
					else
					{
						const char *exp_base = path_basename_ptr(saved[idx].exe_path);
						const char *got_base = path_basename_ptr(exe_read);
						if (exp_base && got_base && strcmp(exp_base, got_base) == 0)
							valid = 1;
					}
				}
				else if (saved[idx].name[0] && comm_read[0])
				{
					if (strcmp(saved[idx].name, comm_read) == 0)
						valid = 1;
				}
				else
				{
					/* no metadata - accept conservatively */
					valid = 1;
				}

				if (!valid)
				{
					/* child doesn't look like expected -> kill and treat as failed */
					kill(newpid, SIGKILL);
					waitpid(newpid, NULL, 0);
					alive = 0;
					printf("Spawn validation failed: newpid=%d exe='%s' comm='%s' expected exe='%s' name='%s'\n",
						   newpid, exe_read[0] ? exe_read : "(none)", comm_read[0] ? comm_read : "(none)",
						   saved[idx].exe_path[0] ? saved[idx].exe_path : "(none)", saved[idx].name);
					printf("Will try to launch restored program in a NEW terminal and release kernel snapshot.\n");

					/* fallback: launch in new terminal */
					launch_in_new_terminal(&saved[idx]);

					struct snap_ioc ioc;
					ioc.oldpid = saved[idx].old_pid;
					ioc.newpid = 0;
					if (ioctl(fd, IOCTL_RESTORE, &ioc) < 0)
						perror("Restore ioctl failed");
					else
						printf("Kernel released snapshot for oldpid=%d (launched in new terminal)\n", saved[idx].old_pid);

					/* remove saved entry locally */
					remove_saved_index(idx);
					continue;
				}
				else
				{
					printf("Spawned new process PID=%d (validated exe/comm)\n", newpid);
				}
			}
			else
			{
				printf("Spawned child PID=%d does not exist or died immediately.\n", newpid);

				/* fallback: launch in new terminal */
				launch_in_new_terminal(&saved[idx]);

				struct snap_ioc ioc;
				ioc.oldpid = saved[idx].old_pid;
				ioc.newpid = 0;
				if (ioctl(fd, IOCTL_RESTORE, &ioc) < 0)
					perror("Restore ioctl failed");
				else
					printf("Kernel released snapshot for oldpid=%d (launched in new terminal)\n", saved[idx].old_pid);

				/* remove saved entry locally */
				remove_saved_index(idx);
				continue;
			}

			// send ioctl - rebind only if alive and validated
			struct snap_ioc ioc;
			ioc.oldpid = oldpid;
			ioc.newpid = alive ? newpid : 0;

			if (ioctl(fd, IOCTL_RESTORE, &ioc) < 0)
			{
				perror("Restore ioctl failed");
			}
			else
			{
				if (ioc.newpid)
					printf("Kernel rebind/restore ok for oldpid=%d -> newpid=%d\n", oldpid, ioc.newpid);
				else
					printf("Kernel released snapshot for oldpid=%d (spawn failed or validation failed)\n", oldpid);
			}

			// remove saved entry from userland table
			remove_saved_index(idx);
		}
		else if (choice == 3)
		{
			if (saved_count == 0)
			{
				printf("No saved processes\n");
				continue;
			}
			printf("\nSaved processes:\n");
			for (int i = 0; i < saved_count; i++)
				printf("[%d] oldPID=%d name=%s exe=%s tty=%s\n", i + 1, saved[i].old_pid, saved[i].name,
					   saved[i].exe_path[0] ? saved[i].exe_path : "(no exe)",
					   saved[i].tty_path[0] ? saved[i].tty_path : "(no tty)");
		}
		else if (choice == 4)
		{
			break;
		}
		else
		{
			printf("Invalid choice\n");
		}
	}

	close(fd);
	return 0;
}
