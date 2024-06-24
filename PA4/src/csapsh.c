//--------------------------------------------------------------------------------------------------
// Shell Lab                               Spring 2024                           System Programming
//
/// @file
/// @brief csapsh - a tiny shell with job control
/// @author Kim Minseo
/// @studid 2020-17429
///
/// @section changelog Change Log
/// 2020/11/14 Bernhard Egger adapted from CS:APP lab
/// 2021/11/03 Bernhard Egger improved for 2021 class
/// 2024/05/11 ARC lab improved for 2024 class
///
/// @section license_section License
/// Copyright CS:APP authors
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// Copyright (c) 2024, Architecture and Code Optimization Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE          // to get basename() in string.h
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jobcontrol.h"
#include "parser.h"

//--------------------------------------------------------------------------------------------------
// Global variables
//

#define P_READ  0
#define P_WRITE 1
char prompt[] = "csapsh> ";  ///< command line prompt (DO NOT CHANGE)
int emit_prompt = 1;         ///< 1: emit prompt; 0: do not emit prompt
int verbose = 0;             ///< 1: verbose mode; 0: normal mode


//--------------------------------------------------------------------------------------------------
// Functions that you need to implement
//
// Refer to the detailed descriptions at each function implementation.

void eval(char *cmdline);
int  builtin_cmd(char *argv[]);
void do_bgfg(char *argv[]);
void waitfg(int jid);
void close_unused_pipes(int** pipes, int num_pipes, int cmd_idx);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);


//--------------------------------------------------------------------------------------------------
// Implemented functions - do not modify
//

// main & helper functions
int main(int argc, char **argv);
void usage(const char *program);
void unix_error(char *msg);
void app_error(char *msg);
void Signal(int signum, void (*handler)(int));
void sigquit_handler(int sig);
char* stripnewline(char *str);

#define VERBOSE(...)  { if (verbose) { fprintf(stderr, ##__VA_ARGS__); fprintf(stderr, "\n"); } }





/// @brief Program entry point.
int main(int argc, char **argv)
{
  char c;
  char cmdline[MAXLINE];

  // redirect stderr to stdout so that the driver will get all output on the pipe connected 
  // to stdout.
  dup2(STDOUT_FILENO, STDERR_FILENO);

  // set Standard I/O's buffering mode for stdout and stderr to line buffering
  // to avoid any discrepancies between running the shell interactively or via the driver
  setlinebuf(stdout);
  setlinebuf(stderr);

  // parse command line
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h': usage(argv[0]);        // print help message
                break;
      case 'v': verbose = 1;           // emit additional diagnostic info
                break;
      case 'p': emit_prompt = 0;       // don't print a prompt
                break;                 // handy for automatic testing
      default:  usage(argv[0]);        // invalid option -> print help message
    }
  }

  // install signal handlers
  VERBOSE("Installing signal handlers...");
  Signal(SIGINT,  sigint_handler);     // Ctrl-c
  Signal(SIGTSTP, sigtstp_handler);    // Ctrl-z
  Signal(SIGCHLD, sigchld_handler);    // Terminated or stopped child
  Signal(SIGQUIT, sigquit_handler);    // Ctrl-Backslash (useful to exit shell)

  // execute read/eval loop
  VERBOSE("Execute read/eval loop...");
  while (1) {
    if (emit_prompt) { printf("%s", prompt); fflush(stdout); }

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
      app_error("fgets error");
    }

    if (feof(stdin)) break;            // end of input (Ctrl-d)

    eval(cmdline);

    fflush(stdout);
  }

  // that's all, folks!
  return EXIT_SUCCESS;
}

void close_unused_pipes(int** pipes, int num_pipes, int cmd_idx) {
  // cmd_idx == 0이면 pipes[0][WRITE] 말고 전부 닫는다
  // cmd_idx == num_pipes-1 이면 pipes[num_pipes-2][READ] 말고 전부 닫는다
  // 그 외의 경우에는 pipes[cmd_idx-1][READ]와 pipes[cmd_idx][WRITE]를 제외하고 전부 닫는다

  for(int i=0; i<num_pipes; i++) {
    if(i == cmd_idx || i == cmd_idx-1) continue;
    close(pipes[i][P_READ]);
    close(pipes[i][P_WRITE]);
  }
  if(cmd_idx < num_pipes-1) close(pipes[cmd_idx][P_READ]);
  if(cmd_idx > 0) close(pipes[cmd_idx-1][P_WRITE]); 
}


/// @brief Evaluate the command line. The function @a parse_cmdline() does the heavy lifting of 
///        parsing the command line and splitting it into separate char **argv[] arrays that 
///        represent individual commands with their arguments. 
///        A command line consists of one job or several jobs connected via ampersand('&'). And
///        a job consists of one process or several processes connected via pipes. Optionally,
///        the output of the entire job can be saved into a file specified by outfile.
///        The shell waits for jobs that are executed in the foreground, while jobs that run
///        in the background are not waited for.
/// @param cmdline command line

void eval(char *cmdline)
{
  #define P_READ  0                      // pipe read end
  #define P_WRITE 1                      // pipe write end

  char *str = strdup(cmdline);
  VERBOSE("eval(%s)", stripnewline(str));
  free(str);

  char ****argv  = NULL; // argv[job_idx] = array of "command line arguments array" seperated by pipes
  char **infile  = NULL; // infile[job_idx] = input redirected file name array (NULL if no input)
  char **outfile = NULL; // outfile[job_idx] = output redirected file name array (NULL if no output)
  char **commands = NULL; // commands[job_idx] = command line string for each job, job is separated by '&'
  int *num_cmds = NULL; // num_cmds[job_idx] = number of commands in each job
  JobState *mode = NULL; // mode[job_idx] = job state (foreground or background)

  // parse command line
  int njob = parse_cmdline(cmdline, &mode, &argv, &infile, &outfile, &num_cmds, &commands);
  VERBOSE("parse_cmdline(...) = %d", njob);
  if (njob == -1) return;              // parse error
  if (njob == 0)  return;              // no input
  assert(njob > 0);

  // dump parsed command line
  for (int job_idx=0; job_idx<njob; job_idx++) {
    if (verbose) dump_cmdstruct(argv[job_idx], infile[job_idx], outfile[job_idx], mode[job_idx]);
  }

  // if the command is a single built-in command (no pipes or redirection), do not fork. Instead,
  // execute the command directly in this process. Note that this is not just to be more efficient -
  // it is necessary for the 'quit' command to work.
  // built-in commands run in foreground, file redirection and pipes are not supported
  if ((njob == 1) && (num_cmds[0] == 1) && (outfile[0] == NULL)) {
    if (builtin_cmd(argv[0][0])) {
      free_cmdstruct(argv, infile, outfile, mode);
      return;
    }
  }

  for(int job_idx = 0; job_idx < njob; job_idx++) {
    int num_cmd = num_cmds[job_idx];
    int **pipefd = NULL;
    int *pid = (int *)calloc(num_cmd, sizeof(int));
    sigset_t set;
    
    if(num_cmd > 1) {
      pipefd = (int **)malloc(sizeof(int *) * (num_cmd-1));
      for(int i = 0; i < num_cmd-1; i++) {
        pipefd[i] = (int *)malloc(sizeof(int) * 2);
        pipe(pipefd[i]);
      }
    }

    // need to block SIGCHILD signal
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);

    for(int cmd_idx = 0; cmd_idx < num_cmd; cmd_idx++) {
      if((pid[cmd_idx] = fork()) == 0) {
        VERBOSE("   Processing %s (%d) ...", argv[job_idx][cmd_idx][0], cmd_idx);
        // input redirection or output redirection
        if(cmd_idx == num_cmd-1) {
          if(infile[job_idx] != NULL) {
            VERBOSE("   %d (%s): Redirecting stdin", cmd_idx, argv[job_idx][cmd_idx][0]);
            int fd = open(infile[job_idx], O_RDONLY);
            dup2(fd, STDIN_FILENO);
            close(fd);
          }
          if(outfile[job_idx] != NULL) {
            VERBOSE("   %d (%s): Redirecting stdout", cmd_idx, argv[job_idx][cmd_idx][0]);
            int fd = open(outfile[job_idx], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd, STDOUT_FILENO);
            close(fd);
          }
        }

        // pipe setting
        if(num_cmd > 1) {
          close_unused_pipes(pipefd, num_cmd-1, cmd_idx);
          if(cmd_idx == 0) {
            // VERBOSE("   stdout -> P_WRITE(%d) of pipe %d", pipefd[0][P_WRITE], 0);
            dup2(pipefd[0][P_WRITE], STDOUT_FILENO);
            close(pipefd[0][P_WRITE]);
          } else if(cmd_idx == num_cmd-1) {
            VERBOSE("   stdin -> P_READ(%d) of pipe %d", pipefd[num_cmd-2][P_READ], num_cmd-2);
            dup2(pipefd[num_cmd-2][P_READ], STDIN_FILENO);
            close(pipefd[num_cmd-2][P_READ]);
          } else {
            VERBOSE("   stdin -> P_READ(%d) of pipe %d", pipefd[cmd_idx-1][P_READ], cmd_idx-1);
            VERBOSE("   stdout -> P_WRITE(%d) of pipe %d", pipefd[cmd_idx][P_WRITE], cmd_idx);
            dup2(pipefd[cmd_idx-1][P_READ], STDIN_FILENO);
            dup2(pipefd[cmd_idx][P_WRITE], STDOUT_FILENO);
            close(pipefd[cmd_idx-1][P_READ]);
            close(pipefd[cmd_idx][P_WRITE]);
          }
        }

        // place in new process group
        if(cmd_idx == 0) {
          setpgid(0,0);
        } else {
          while(pid[0]==0) usleep(1);
          setpgid(0, pid[0]);
        }

        // unblock SIGCHILD signal
        sigprocmask(SIG_UNBLOCK, &set, NULL);

        if(builtin_cmd(argv[job_idx][cmd_idx])) {
          exit(0);
        }

        // execute command
        execvp(argv[job_idx][cmd_idx][0], argv[job_idx][cmd_idx]);

        printf("No such file or directory\n");
        exit(1);
      }
    }
    for(int i = 0; i < num_cmd-1; i++) {
      close(pipefd[i][P_READ]);
      close(pipefd[i][P_WRITE]);
    }

    // add job
    int jid = addjob(pid[0], pid, num_cmd, mode[job_idx], commands[job_idx]);
    // unblock SIGCHILD signal
    sigprocmask(SIG_UNBLOCK, &set, NULL);

    // wait for foreground job, doesn't wait for background job
    if(mode[job_idx] == jsForeground) {
      waitfg(jid);
    } else {
      printjob(jid);
    }
      

    // free memory    
    for(int i=0; i<num_cmd-1; i++) {
      free(pipefd[i]);
    }
    free(pipefd);
  }
}


/// @brief Execute built-in commands
/// @param argv command
/// @retval 1 if the command was a built-in command
/// @retval 0 otherwise
int builtin_cmd(char *argv[])
{
  VERBOSE("builtin_cmd(%s)", argv[0]);
  if(strcmp(argv[0], "quit") == 0) exit(EXIT_SUCCESS);
  else if(strcmp(argv[0], "jobs") == 0) listjobs();
  else if(strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) do_bgfg(argv);
  else return 0;

  return 1;
}

/// @brief Execute the builtin bg and fg commands
/// @param argv char* argv[] array where 
///           argv[0] is either "bg" or "fg"
///           argv[1] is either a job id "%<n>", a process group id "@<n>" or a process id "<n>"
void do_bgfg(char *argv[])
{
  VERBOSE("do_bgfg(%s, %s)", argv[0], argv[1]);

  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }

  int id;
  Job* job;

  if(argv[1][0] == '%') { // jid
    id = atoi(&argv[1][1]);
    job = getjob_jid(id);
    if(!job) {
      printf("[%%%d]: No such job\n", id);
      return;
    }
  } else if(argv[1][0] == '@') { // pgid
    id = atoi(&argv[1][1]);
    job = getjob_pgid(id);
    if(!job) {
      printf("(@%d): No such process group\n", id);
      return;
    }
  } else { // pid
    id = atoi(argv[1]); // return 0 if not a number, there's no pid 0 in user process
    job = getjob_pid(id);
    if(!job) {
      printf("{%d}: No such process\n", id);
      return;
    }
  }

  if(job->state == jsStopped) { // send SIGCONT if stopped
    if(kill(-1 * job->pgid, SIGCONT) < 0) {
      perror("[do_bgfg] kill failed");
    }
  }

  if(strcmp(argv[0], "bg") == 0) {
    job->state = jsBackground;
    printjob(job->jid);
    VERBOSE("[%d] (%d) {%d} Running %s", job->jid, job->pid, job->pgid, job->cmdline); 
  } else {
    job->state = jsForeground;
    VERBOSE("[%d] (%d) {%d} Foreground %s", job->jid, job->pid, job->pgid, job->cmdline);
    waitfg(job->jid);
  }
}

/// @brief Block until job jid is no longer in the foreground
/// @param jid job ID of foreground job
void waitfg(int jid)
{
  if (verbose) {
    fprintf(stderr, "waitfg(%%%d): ", jid);
    printjob(jid);
  }
  
  // use a buy loop to wait for the job to finish
  while(1) {
    Job *job = getjob_jid(jid);
    if(job == NULL || job->state != jsForeground)
      break;
    usleep(1);
  }
}


//--------------------------------------------------------------------------------------------------
// Signal handlers
//

/// @brief SIGCHLD handler. Sent to the shell whenever a child process terminates or stops because
///        it received a SIGSTOP or SIGTSTP signal. This handler reaps all zombies.
/// @param sig signal (SIGCHLD)
void sigchld_handler(int sig)
{
  VERBOSE("[SCH] SIGCHLD handler (signal: %d)", sig);

  int status;
  pid_t wpid;
  while((wpid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
    VERBOSE("[SCH]\tWaitpid returned %d", wpid);
    Job* job = getjob_pid(wpid);

    if(WIFEXITED(status)) {
      VERBOSE("[SCH]\tProcess %d terminated normally.", wpid);
      
      // delete job
      if(job) {
        job->nproc_cur--;
        if(job->nproc_cur == 0) {
          job->state = jsUndefined;
          VERBOSE("[SCH]\tJob [%%%d] deleted.", job->jid);
          deletejob(job->jid);
        }
      }
    } else if(WIFSTOPPED(status)) {
      VERBOSE("[SCH]\tProcess %d stopped by signal %d.", wpid, WSTOPSIG(status));

      // update job state
      if(job && job->state != jsStopped) {
        job->state = jsStopped;
      }
      if(kill(-1 * job->pgid, SIGSTOP) < 0) {
        perror("[SCH] kill failed");
      }
    } else if(WIFCONTINUED(status)) {
      // don't know why but ref execution result says signal number is 255
      VERBOSE("[SCH]\tProcess %d continued by signal %d.", wpid, 255);
      if(kill(-1 * job->pgid, SIGCONT) < 0) {
        perror("[SCH] kill failed");
      }
    } else {
      VERBOSE("[SCH]\tProcess %d terminated by signal %d.", wpid, WTERMSIG(status));

      // delete job
      if(job) {
        job->nproc_cur--;
        if(job->nproc_cur == 0) {
          job->state = jsUndefined;
          VERBOSE("[SCH]\tJob [%%%d] deleted.", job->jid);
          deletejob(job->jid);
        }
      }
    }
  }
}

/// @brief SIGINT handler. Sent to the shell whenever the user types Ctrl-c at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGINT)
void sigint_handler(int sig)
{
  VERBOSE("[SIH] SIGINT handler (signal: %d)", sig);

  Job* job = getjob_foreground();
  if(!job) {
    VERBOSE("[SIH]\tJob ID of foreground process is %%-1.");
    return;
  }

  VERBOSE("[SIH]\tJob ID of foreground process is %%%d.", job->jid);
  if(kill(-1*job->pgid, SIGINT) < 0) {
    perror("[SIH] kill error");
  }
}

/// @brief SIGTSTP handler. Sent to the shell whenever the user types Ctrl-z at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGTSTP)
void sigtstp_handler(int sig)
{
  VERBOSE("[SSH] SIGTSTP handler (signal: %d)", sig);

  Job* job = getjob_foreground();
  if(!job) {
    VERBOSE("[SSH]\tJob ID of foreground process is %%-1.");
    return;
  }

  VERBOSE("[SSH]\tJob ID of foreground process is %%%d.", job->jid);
  if(kill(-1*job->pgid, SIGTSTP) < 0) {
    perror("[SSH] kill error");
  }
}


//--------------------------------------------------------------------------------------------------
// Other helper functions
//

/// @brief Print help message. Does not return.
__attribute__((noreturn))
void usage(const char *program)
{
  printf("Usage: %s [-hvp]\n", basename(program));
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(EXIT_FAILURE);
}

/// @brief Print a Unix-level error message based on errno. Does not return.
/// param msg additional descriptive string (optional)
__attribute__((noreturn))
void unix_error(char *msg)
{
  if (msg != NULL) fprintf(stdout, "%s: ", msg);
  fprintf(stdout, "%s\n", strerror(errno));
  exit(EXIT_FAILURE);
}

/// @brief Print an application-level error message. Does not return.
/// @param msg error message
__attribute__((noreturn))
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(EXIT_FAILURE);
}

/// @brief Wrapper for sigaction(). Installs the function @a handler as the signal handler
///        for signal @a signum. Does not return on error.
/// @param signum signal number to catch
/// @param handler signal handler to invoke
void Signal(int signum, void (*handler)(int))
{
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); // block sigs of type being handled
  action.sa_flags = SA_RESTART; // restart syscalls if possible

  if (sigaction(signum, &action, NULL) < 0) unix_error("Sigaction");
}

/// @brief SIGQUIT handler. Terminates the shell.
__attribute__((noreturn))
void sigquit_handler(int sig)
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(EXIT_SUCCESS);
}

/// @brief strip newlines (\n) from a string. Warning: modifies the string itself!
///        Inside the string, newlines are replaced with a space, at the end 
///        of the string, the newline is deleted.
///
/// @param str string
/// @reval char* stripped string
char* stripnewline(char *str)
{
  char *p = str;
  while (*p != '\0') {
    if (*p == '\n') *p = *(p+1) == '\0' ? '\0' : ' ';
    p++;
  }

  return str;
}
