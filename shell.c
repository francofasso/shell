#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "parser/ast.h"
#include "shell.h"

void initialize(void)
{
    /* This code will be called once at startup */
    if (prompt)
        prompt = "vush$ ";
    
    // Ignore signals in the shell
    signal(SIGINT, SIG_IGN);
    // Ignore quit signal
    signal(SIGQUIT, SIG_IGN);
    // Ignore stop signal
    signal(SIGTSTP, SIG_IGN);
}

void execute_command(node_t *node) {
    char *program = node->command.program;
    char **argv = node->command.argv;
    int argc = node->command.argc;
    
    // Special case, built-in commands
    if (strcmp(program, "exit") == 0) {
        int exit_code = 0;
        
        // Check for an exit code argument
        if (argc > 1) {
            exit_code = atoi(argv[1]);
        }
        
        exit(exit_code);
    }
    
    if (strcmp(program, "cd") == 0) {
        char *path;
        
        // Get target directory
        if (argc < 2) {
            path = getenv("HOME");
            if (path == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                return;
            }
        }
        else {
            path = argv[1];
        }
        
        if (chdir(path) < 0) {
            perror("cd");
        }
        
        return;
    }
    
    // Base case
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return;
    }
    else if (pid == 0) {
        // restore default signal handlers
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        
        // Child Process, execute
        execvp(program, argv);
        
        // execvp failed
        perror(program);
        exit(1);
    }
    else {
        // Parent Process, waits for the child to finish
        int status;
        waitpid(pid, &status, 0);
    }
}

void execute_sequence(node_t *node) {
    run_command(node->sequence.first);
    run_command(node->sequence.second);
}

void execute_pipe(node_t *node) {
    int n = node->pipe.n_parts;
    
    // Array of PIDs of child processes
    pid_t *pids = malloc(n * sizeof(pid_t));
    
    // Array of pipes    
    int (*pipes)[2] = malloc((n - 1) * sizeof(int[2]));
    
    // Creates the pipes   
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            free(pids);
            free(pipes);
            return;
        }
    }
    
    // Creates a process for each command
    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        
        if (pids[i] < 0) {
            perror("fork");

            // Cleanup
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            free(pids);
            free(pipes);
            return;
        }
        
        if (pids[i] == 0) {
            // If not first command, read from previous pipe
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            
            // If not last command, write to next pipe
            if (i < n - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            // Execute the command
            node_t *cmd = node->pipe.parts[i];
            
            if (cmd->type == NODE_COMMAND) {
                execvp(cmd->command.program, cmd->command.argv);
                perror(cmd->command.program);
                exit(1);
            }
            else {
                // if it's not a simple command, recursion
                run_command(cmd);
                exit(0);
            }
        }
    }

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], NULL, 0);
    }
    
    free(pids);
    free(pipes);
}

void execute_redirect(node_t *node) {    
    node_t *child = node->redirect.child;
    int fd = node->redirect.fd;
    int mode = node->redirect.mode;
    char *target = node->redirect.target;
    
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return;
    }
    
    if (pid == 0) { 
        // child process, restores default signal handlers       
        int file_fd;
        
        switch (mode) {
            case REDIRECT_INPUT:
                file_fd = open(target, O_RDONLY);
                if (file_fd < 0) {
                    perror(target);
                    exit(1);
                }
                
                if (dup2(file_fd, STDIN_FILENO) < 0) {
                    perror("dup2");
                    exit(1);
                }
                
                close(file_fd);
                break;
                
            case REDIRECT_OUTPUT:
                // case >
                file_fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (file_fd < 0) {
                    perror(target);
                    exit(1);
                }
                
                int target_fd = (fd < 0) ? STDOUT_FILENO : fd;
                
                if (dup2(file_fd, target_fd) < 0) {
                    perror("dup2");
                    exit(1);
                }
                
                if (fd < 0) {
                    if (dup2(file_fd, STDERR_FILENO) < 0) {
                        perror("dup2");
                        exit(1);
                    }
                }
                
                close(file_fd);
                break;
                
            case REDIRECT_APPEND:
                // case >>
                file_fd = open(target, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (file_fd < 0) {
                    perror(target);
                    exit(1);
                }
                
                target_fd = (fd < 0) ? STDOUT_FILENO : fd;
                
                if (dup2(file_fd, target_fd) < 0) {
                    perror("dup2");
                    exit(1);
                }
                
                if (fd < 0) {
                    if (dup2(file_fd, STDERR_FILENO) < 0) {
                        perror("dup2");
                        exit(1);
                    }
                }
                
                close(file_fd);
                break;
                
            case REDIRECT_DUP:
                // case >& o <&
                if (dup2(node->redirect.fd2, fd) < 0) {
                    perror("dup2");
                    exit(1);
                }
                break;
                
            default:
                fprintf(stderr, "Unknown redirect type: %d\n", mode);
                exit(1);
        }

        // Execute the command with the active redirection
        run_command(child);
        exit(0);
    }
    else {
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
        }
    }
}

void run_command(node_t *node)
{
    if (node == NULL) {
        return;
    }
    
    switch (node->type)
    {
    case NODE_COMMAND:
        execute_command(node);
        break;
        
    case NODE_SEQUENCE:
        execute_sequence(node);
        break;
        
    case NODE_PIPE:
        execute_pipe(node);
        break;
        
    case NODE_REDIRECT:
        execute_redirect(node);
        break;
        
    case NODE_SUBSHELL:
        fprintf(stderr, "not yet implemented\n");
        break;
        
    case NODE_DETACH:
        fprintf(stderr, "not yet implemented\n");
        break;
    
    default:
        fprintf(stderr, "Unknown node type: %d\n", node->type);
        break;
    }
}
