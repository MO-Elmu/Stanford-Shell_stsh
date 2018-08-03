/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iomanip> // for setw
#include <assert.h>
#include <cstring>
#include <cerrno>
#include <string.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it

struct Pipe
{
   int fd[2];   // represent a pipe
};


/* Functions signatures */
static void updateJobList(STSHJobList& jobList, pid_t pid, STSHProcessState state);
static void giveTermCtrl (pid_t gpid);

/*All shell Builtin Functions*/

static void fg(size_t jobNumber) {
  if(joblist.containsJob(jobNumber)) {
     sigset_t mask, prev;
     sigemptyset(&mask);
     sigaddset(&mask, SIGCHLD);
     sigprocmask(SIG_BLOCK, &mask, &prev);  // Block SIGCHLD
     STSHJob& job = joblist.getJob(jobNumber);
     pid_t gpid = job.getGroupID();
     STSHProcess& process = job.getProcess(gpid);
     kill(gpid, SIGCONT);
     giveTermCtrl(gpid);
     while(process.getState() != kTerminated) sigsuspend(&prev);
     giveTermCtrl(getpid());
     sigprocmask(SIG_UNBLOCK, &mask, NULL);

  }
 else {
      //courtesy of Ben stackoverflow
      stringstream errMsg;
      errMsg << "fg " << jobNumber << ": No such job.";
      throw STSHException(errMsg.str().c_str());

 }
}
static void bg(size_t jobNumber) {
  if(joblist.containsJob(jobNumber) && joblist.hasForegroundJob()) {
     STSHJob& job = joblist.getJob(jobNumber);
     pid_t gpid = job.getGroupID();
     updateJobList(joblist, gpid, kRunning);
     giveTermCtrl(getpid());
  }
 else {
      //courtesy of Ben stackoverflow
      stringstream errMsg;
      errMsg << "bg " << jobNumber << ": No such job.";
      throw STSHException(errMsg.str().c_str());
 }
}

static void slay(pid_t pid) {
  if(joblist.containsProcess(pid)) {
     kill(pid, SIGKILL);
  }
  else {
      stringstream errMsg;
      errMsg << "No process with pid " << pid ;
      throw STSHException(errMsg.str().c_str());
 }
}
static void halt(pid_t pid) {
  if(joblist.containsProcess(pid)) {
     kill(pid, SIGSTOP);
  }
  else {
      stringstream errMsg;
      errMsg << "No process with pid " << pid ;
      throw STSHException(errMsg.str().c_str());
 }
}
static void cont(pid_t pid) {
  if(joblist.containsProcess(pid)) {
     kill(pid, SIGCONT);
  }
  else {
      stringstream errMsg;
      errMsg << "No process with pid " << pid ;
      throw STSHException(errMsg.str().c_str());
 }
}

/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);
static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  size_t index = iter - kSupportedBuiltins;
  pid_t pid;
  size_t jobNo;
  switch (index) {
  case 0:
  case 1: exit(0);
  case 2:
          try {
              jobNo = stoi (pipeline.commands[0].tokens[0]);
          }catch(...){
              cout << "Usage: fg <jobid>." << endl;
              break;
          }
          fg(jobNo);
          break;
  case 3:
          try {
              jobNo = stoi (pipeline.commands[0].tokens[0]);
          }catch(...){
              cout << "Usage: bg <jobid>." << endl;
              break;
          }
          bg(jobNo);
          break;
  case 4:
          try {
              pid = stoi (pipeline.commands[0].tokens[0]);
          }catch(...){
              cout << "Usage: slay <jobid> <index> | <pid>." << endl;
              break;
          }
          slay(pid);
          break;
  case 5:
          try {
              pid = stoi (pipeline.commands[0].tokens[0]);
          }catch(...){
              cout << "Usage: halt <jobid> <index> | <pid>." << endl;
              break;
          }
          halt(pid);
          break;
  case 6:
          try {
              pid = stoi (pipeline.commands[0].tokens[0]);
          }catch(...){
              cout << "Usage: cont <jobid> <index> | <pid>." << endl;
              break;
          }
          cont(pid);
          break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }

  return true;
}

/**
 * Function: handleBuiltin
 * -----------------------
 * Updates the joblist according to a state change for a specific process
 */

static void updateJobList(STSHJobList& jobList, pid_t pid, STSHProcessState state) {

     if (!jobList.containsProcess(pid)) return;
     STSHJob& job = joblist.getJobWithProcess(pid);
     assert(job.containsProcess(pid));
     STSHProcess& process = job.getProcess(pid);
     process.setState(state);
     jobList.synchronize(job);
}
/**
 * Function: sigchild_handler
 * --------------------------
 * Handles the SIGCHLD signal for the stsh
 */

static void sigchild_handler(int sig) {
  pid_t child_pid;
  int status;
  child_pid = waitpid(-1, &status, WNOHANG|WUNTRACED|WCONTINUED);
  if(WIFEXITED(status)) updateJobList(joblist, child_pid, kTerminated);
  if(WIFSIGNALED(status)) updateJobList(joblist, child_pid, kTerminated);
  if(WIFSTOPPED(status)) updateJobList(joblist, child_pid, kStopped);
  if(WIFCONTINUED(status)) updateJobList(joblist, child_pid, kRunning);
}
static void sigint_handler(int sig){
  if (joblist.hasForegroundJob()){
     STSHJob& job = joblist.getForegroundJob();
     pid_t pid = job.getGroupID();
     kill(pid, SIGINT);
     cout << endl;
  }
}

static void sigtstp_handler(int sig){
  if (joblist.hasForegroundJob()){
     STSHJob& job = joblist.getForegroundJob();
     pid_t pid = job.getGroupID();
     kill(pid, SIGTSTP);
     cout << endl;
  }
}

/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD,
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and
 * ignores two others.
 */
static void installSignalHandlers() {
  installSignalHandler(SIGQUIT, [](int sig) { exit(0); });
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
  installSignalHandler(SIGCHLD, sigchild_handler);
  installSignalHandler(SIGINT, sigint_handler);
  installSignalHandler(SIGTSTP, sigtstp_handler);
}

/* Helper function to give terminal control to a job using "tcsetpgrp" function */
static void giveTermCtrl (pid_t gpid) {
  if(tcsetpgrp(STDOUT_FILENO,gpid) < 0 && errno != ENOTTY){
     throw STSHException("Error: Control Transfer Failed");
  }
  if(tcsetpgrp(STDIN_FILENO,gpid) < 0 && errno != ENOTTY){
     throw STSHException("Error: Control Transfer Failed");
  }
}

/* Helper Function to manage input/output redirection */
static void redirect (const pipeline& p, bool inputFile_exist, bool outputFile_exist){
	if (inputFile_exist){
                const char *fileNameIN = p.input.c_str();
                int fd_in;
		if((fd_in = open(fileNameIN, O_RDONLY, 0)) < 0){
                   if(errno == 2) cout << "Could not open \"" << p.input <<"\"."  << endl;
                   else cout << strerror(errno) << endl;
                   exit(0);
                }else {
		   dup2(fd_in, STDIN_FILENO);
                   close(fd_in);
                }
	}
	if (outputFile_exist){
                const char *fileNameOUT = p.output.c_str();
                int fd_out;
		if((fd_out = open(fileNameOUT, O_CREAT|O_TRUNC|O_WRONLY, 0644)) < 0){
                   cout << strerror(errno) << endl;
                   exit(0); 
                }else {
		   dup2(fd_out, STDOUT_FILENO);
                   close(fd_out);
                }
	}
}	

/**
 * Function: createPipes
 * -------------------
 * Creates number of pipes equal to (num of commands - 1)in the pipeline.
 */
static vector<Pipe> createPipes (int numOfPipes){
   
   vector<Pipe> pipes (numOfPipes);    
   for(int i = 0; i < numOfPipes; i++){
     Pipe p;
     pipe(p.fd);
     pipes[i] = p;
   }
   return pipes;
}
/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
    
    STSHJob& job = joblist.addJob(kBackground);
    sigset_t mask, prev;
    if (!p.background){
         job.setState(kForeground);
         sigemptyset(&mask);
         sigaddset(&mask, SIGCHLD);
         sigprocmask(SIG_BLOCK, &mask, &prev);  // Block SIGCHLD
    }
    pid_t first_child;   //pid of the leading process in the pipeline
    size_t pipe_len = p.commands.size();
    vector<Pipe> pipes = createPipes (pipe_len-1);  //create pipes
    pid_t pid;    
    for(size_t i = 0; i < pipe_len; i++) { //loop through the pipe and fork children accordingly
        pid = fork();
        if(i == 0)first_child = pid;
        if(pid == 0) {
           if(i == 0) {
              first_child = getpid();
              if(pipe_len > 1){            //means there are more than one command in the pipeline
                dup2(pipes[i].fd[1], STDOUT_FILENO);
                redirect (p, !p.input.empty(), false);
                for (auto p : pipes) {close(p.fd[0]); close(p.fd[1]);}    //close all pipes
              }else{                      //means there is no piping and only one command in the pipline
                redirect (p, !p.input.empty(), !p.output.empty());
                for (auto p : pipes) {close(p.fd[0]); close(p.fd[1]);}
              }
           }
           else if(i == (pipe_len-1)) {   //means the last command on the pipe
                  dup2(pipes[i-1].fd[0], STDIN_FILENO);
                  redirect (p, false, !p.output.empty());
                  for (auto p : pipes) {close(p.fd[0]); close(p.fd[1]);}   //close all pipes
           }
           else {
                  dup2(pipes[i-1].fd[0], STDIN_FILENO);
                  dup2(pipes[i].fd[1], STDOUT_FILENO);
                  for (auto p : pipes) {close(p.fd[0]); close(p.fd[1]);}  //close all pipes
           }
           if (setpgid(0,first_child) < 0) cerr << strerror(errno) << endl;
           char *argv[kMaxArguments + 2];
           argv[0] = (char *) p.commands[i].command;
           memcpy(argv+1,p.commands[i].tokens,(kMaxArguments+1)*sizeof(char*));
           execvp(argv[0],argv);
           cerr << argv[0] << ": Command not found." << endl;
           exit(0);
        }
        if(setpgid(pid,first_child) < 0) cerr << strerror(errno) << endl;
        job.addProcess(STSHProcess(pid,p.commands[i]));
    }
    for (auto p : pipes) {close(p.fd[0]); close(p.fd[1]);}  //close all pipes in parent shell
    if (!p.background){
       giveTermCtrl(pid);
       while(job.getState() == kForeground) sigsuspend(&prev);
       giveTermCtrl(getpid());
       sigprocmask(SIG_UNBLOCK, &mask, NULL);
    }
    else{
       vector<STSHProcess>& procs = job.getProcesses();
       ostringstream oss;
       oss << "[" << job.getNum() << "]";
       cout << setw(1) << oss.str();
       for (size_t i = 0; i < procs.size(); i++) {
           cout << setw(1) << " " << procs[i].getID();
       }
       cout << endl;
    }
 }

/**
 * Function: main
 * --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv);
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }
  return 0;
}
