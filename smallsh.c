#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>

bool isForegroundOnly = false;
char status[30] = "exit value 0";

// Structure used for dynamic array
// Outline taken from https://stackoverflow.com/questions/3536153/c-dynamically-growing-array
typedef struct arrayStruct
{
    int *array;
    size_t capacity;
    size_t size;
} array;

// Initialize an array
// Outline taken from https://stackoverflow.com/questions/3536153/c-dynamically-growing-array
void arrayInit(array *arr, size_t capacity)
{
    arr->array = calloc(capacity, sizeof(int));
    arr->capacity = capacity;
    arr->size = 0;
}

// Insert a new element at the end of an array
// Outline taken from https://stackoverflow.com/questions/3536153/c-dynamically-growing-array
void arrayInsert(array *arr, int num)
{
    // Resize array if full
    if (arr->size >= arr->capacity)
    {
        arr->capacity *= 2;
        arr->array = realloc(arr->array, arr->capacity * sizeof(int));
    }

    // Insert the new element
    arr->array[arr->size] = num;
    arr->size += 1;
}

// Removes first instance of number from an array, does nothing if number not found
void arrayRemove(array *arr, int num)
{
    bool elementFound = false;

    // Loop through the elements
    int i;
    for (i = 0; i < arr->size - 1; i++)
    {
        // Move values to left if element has been found
        if (elementFound)
        {
            arr->array[i] = arr->array[i + 1];
        }
        // Move values to left when element is found
        else if (arr->array[i] == num && !elementFound)
        {
            elementFound = true;
            arr->array[i] = arr->array[i + 1];
        }
    }

    // Reduce size of array if number was in array
    if (elementFound || arr->array[arr->size - 1])
    {
        arr->size -= 1;
    }
}

// Frees an array from memory
// Outline taken from https://stackoverflow.com/questions/3536153/c-dynamically-growing-array
void freeArray(array *arr)
{
    // Free all memory
    free(arr->array);
    arr->array = NULL;
    arr->capacity = 0;
    arr->size = 0;
}

// Initialize an array to keep track of running processes
array runningProcesses;

// Handler for SIGNINT
void handle_SIGINT(int signo)
{
    return;
}

// Handler for SIGNSTP
void handle_SIGTSTP(int signo)
{
    // Disable foreground only mode
    if (isForegroundOnly)
    {
        char *message = "Exiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 29);
        isForegroundOnly = false;
    }
    // Enable foreground only mode
    else
    {
        char *message = "Entering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 49);
        isForegroundOnly = true;
    }
}

// Checks if output is to be redirected
bool pidCheck(char *input)
{
    return strstr(input, "$$") != NULL;
}

// Checks if output is to be redirected
bool outputCheck(char *input)
{
    return strchr(input, '>') != NULL;
}

// Checks if input is to be redirected
bool inputCheck(char *input)
{
    return strchr(input, '<') != NULL;
}

// Checks if input is to be run in background
bool backgroundCheck(char *input)
{
    // Get length of string
    size_t length = strlen(input);

    // Check if last character before \n is &
    return input[length - 2] == '&' && isForegroundOnly == false;
}

// Prints the status when status is called
void printStatus()
{
    printf("%s\n", status);
    fflush(stdout);
}

// Changes the directory when cd is called
void changeDirectory(char *input)
{
    // Break the string into section divided by a space
    char *token = strtok(input, " ");

    // Get the second token as we know the first is cd
    token = strtok(NULL, " \n");

    // Set directory to home directory if no path is given
    if (token == NULL || token == "&")
    {
        chdir(getenv("HOME"));
    }
    // Set directory to path given
    else
    {
        char path[PATH_MAX];

        // Check whether path is absolute
        if (strncmp(token, "/", 1) == 0)
        {
            // Path is absolute
            // Copy the given directory to path
            strcpy(path, token);
        }
        else
        {
            // Path is relative
            // Get the current directory
            getcwd(path, sizeof(path));

            // Copy the argument to end of path
            strcat(path, "/");
            strcat(path, token);
        }

        // Change the directory to the path
        int status = chdir(path);

        // Check if directory changed successfully
        switch (status)
        {
        // directory did not change successfully
        case -1:
            printf("%s: %s\n", path, strerror(errno));
            fflush(stdout);
            return;
            break;
        }
    }
}

// Parses input for command, arguments, input redirection, and output redirection
// Returns number of arguments
int parseInput(char *input, char **command, char *args[512], char **infile, char **outfile)
{
    int numArgs = 0;

    // Allocate memory for copy of input
    char *parsedInput;
    parsedInput = (char *)malloc(strlen(input) * sizeof(char) + 5);
    strcpy(parsedInput, input);

    // Get command
    char *token = strtok(parsedInput, " \n");
    *command = (char *)malloc(strlen(token) * sizeof(char) + 5);
    strcpy(*command, token);

    token = strtok(NULL, " \n");

    while (token != NULL)
    {
        // Check for input redirection
        if (strcmp(token, "<") == 0)
        {
            // Copy input file to infile
            token = strtok(NULL, " \n");
            *infile = (char *)malloc(strlen(token) * sizeof(char) + 5);
            strcpy(*infile, token);
        }
        else if (strcmp(token, ">") == 0)
        {
            // Copy output file to output
            token = strtok(NULL, " \n");
            *outfile = (char *)malloc(strlen(token) * sizeof(char) + 5);
            strcpy(*outfile, token);
        }
        else if (strcmp(token, "&") != 0)
        {
            // Copy argument to args
            args[numArgs] = (char *)malloc(strlen(token) * sizeof(char) + 5);
            strcpy(args[numArgs], token);
            numArgs++;
        }

        token = strtok(NULL, " \n");
    }

    free(parsedInput);
    return numArgs;
}

// Replaces $$ with the pid
void replacePid(char *input)
{
    size_t maxSize = 2048;
    char buffer[maxSize];
    char *ptr;

    // Get pointer of where $$ appears
    ptr = strstr(input, "$$");

    // Copy characters from start of input to $$
    strncpy(buffer, input, ptr - input);
    buffer[ptr - input] = '\0';

    // Copy rest of input to buffer
    sprintf(buffer + (ptr - input), "%d%s", getpid(), ptr + strlen("$$"));

    // Copy buffer back into input
    strcpy(input, buffer);
}

int main(void)
{
    arrayInit(&runningProcesses, 4);

    // Signal handler api taken from Exploration: Signal Handling API on canvas
    // Set up signal handlers
    struct sigaction SIGINT_action = {0}, SIGTSTP_action = {0};

    // Fill out the SIGINT_action struct
    // Register handle_SIGINT as the signal handler
    SIGINT_action.sa_handler = handle_SIGINT;
    // Block all catchable signals while handle_SIGINT is running
    sigfillset(&SIGINT_action.sa_mask);
    // No flags set
    SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);

    // Fill out the SIGTSTP_action struct
    // Register handle_SIGTSTP as the signal handler
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    // Block all catchable signals while handle_SIGINT is running
    sigfillset(&SIGTSTP_action.sa_mask);
    // No flags set
    SIGTSTP_action.sa_flags = 0;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // Start the shell
    while (true)
    {
        // Allocate space for user input
        char *input;
        size_t maxSize = 2048;
        input = (char *)malloc(maxSize * sizeof(char));

        // Test for completed processes
        int childStatus;
        pid_t spawnPid;
        int i;
        for (i = 0; i < runningProcesses.size; i++)
        {
            spawnPid = waitpid(runningProcesses.array[i], &childStatus, WNOHANG);
            if (spawnPid != 0)
            {
                // Display message showing return or termination value
                printf("background pid %d is done: ", spawnPid);
                fflush(stdout);
                if (WIFEXITED(childStatus))
                {
                    printf("exit value %d\n", WEXITSTATUS(childStatus));
                    fflush(stdout);
                }
                else
                {
                    printf("terminated by signal %d\n", WTERMSIG(childStatus));
                    fflush(stdout);
                }

                arrayRemove(&runningProcesses, spawnPid);
            }
        }

        // Display the prompt and get input from the user
        printf(": ");
        fflush(stdout);
        getline(&input, &maxSize, stdin);

        // Check if comment or blank line
        if (input[0] == '#' || input[0] == '\n')
        {
            continue;
        }

        // Initialize parts of input
        char *command = NULL;
        char *args[512] = {NULL};
        char *infile = NULL;
        char *outfile = NULL;

        // Check if $$ is in input and needs to be replaced
        if (pidCheck(input))
        {
            replacePid(input);
        }

        // Parse the input
        int numArgs = parseInput(input, &command, args, &infile, &outfile);

        // Check if any of the built-in commands were called
        if (strcmp(command, "exit") == 0)
        {
            // exit is called
            free(input);
            free(command);
            break;
        }
        else if (strcmp(command, "cd") == 0)
        {
            // cd is called
            changeDirectory(input);
        }
        else if (strcmp(command, "status") == 0)
        {
            // status is called
            printStatus();
        }
        else
        {
            // Command is not built-in
            // Check if process is to run in background and if I/O is rerouted
            bool background = backgroundCheck(input);

            // Build up argument list for execvp
            char *argv[numArgs + 2];
            // Copy command
            argv[0] = (char *)malloc(strlen(command) * sizeof(char) + 5);
            strcpy(argv[0], command);
            // Copy arguments
            int i;
            for (i = 1; i <= numArgs; i++)
            {
                argv[i] = (char *)malloc(strlen(args[i - 1]) * sizeof(char) + 5);
                strcpy(argv[i], args[i - 1]);
            }
            // Copy NULL to end
            argv[numArgs + 1] = NULL;

            // Fork a new process
            int childStatus;
            pid_t spawnPid = fork();

            switch (spawnPid)
            {
            case -1:
                // Fork fails
                perror("fork()\n");
                exit(1);
                break;
            case 0:
                // In the child process
                // Set default I/O for background processes
                if (background)
                {
                    int sourceFD = open("/dev/null", O_RDONLY);
                    int targetFD = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    int result = dup2(sourceFD, 0);
                    result = dup2(targetFD, 1);
                }

                // Redirect input if provided
                if (infile != NULL)
                {
                    int sourceFD = open(infile, O_RDONLY);
                    if (sourceFD == -1)
                    {
                        perror(infile);
                        exit(1);
                    }
                    int result = dup2(sourceFD, 0);
                }

                // Redirect output if provided
                if (outfile != NULL)
                {
                    int targetFD = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (targetFD == -1)
                    {
                        perror(outfile);
                        exit(1);
                    }
                    int result = dup2(targetFD, 1);
                }

                // Run the command
                execvp(argv[0], argv);

                // exec only returns if there is an error
                perror(command);
                fflush(stdout);
                strcpy(status, "exit value 1");
                exit(1);
                break;
            default:
                // In the parent process
                if (!background)
                {
                    // Wait on the child process
                    arrayInsert(&runningProcesses, spawnPid);
                    spawnPid = waitpid(spawnPid, &childStatus, 0);
                    arrayRemove(&runningProcesses, spawnPid);

                    // Set status based on child termination
                    if (WIFEXITED(childStatus))
                    {
                        sprintf(status, "exit value %d", WEXITSTATUS(childStatus));
                    }
                    else
                    {
                        sprintf("terminated by signal %d", WTERMSIG(childStatus));
                    }
                }
                else
                {
                    // Insert pid into array
                    arrayInsert(&runningProcesses, spawnPid);
                    printf("background pid is %d\n", spawnPid);
                    fflush(stdout);
                }

                break;
            }

            // Free argument list
            for (i = 0; i <= numArgs; i++)
            {
                free(argv[i]);
            }
        }

        // Free allocated memory
        if (inputCheck(input))
            free(infile);
        if (outputCheck(input))
            free(outfile);
        free(input);
        free(command);
        int j;
        for (j = 0; j < numArgs; j++)
        {
            free(args[j]);
        }
    }

    freeArray(&runningProcesses);
    return 0;
}