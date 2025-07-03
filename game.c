/*
 * game.c - This is the Dungeon Master process.
 * It sets up shared memory and semaphores, forks character processes,
 * and then calls the RunDungeon function from the provided dungeon.o binary
 * to manage the game loop and challenges. It also handles cleanup.
 */

// Include necessary headers for system calls and standard libraries.
// Ensure POSIX feature test macros are defined before includes if needed by your environment.
#define _XOPEN_SOURCE 700       // Ensure POSIX features are available
#define _POSIX_C_SOURCE 200809L // Ensure POSIX.1-2008 compliance

#include <stdio.h>      // For printf, perror
#include <stdlib.h>     // For exit(), EXIT_FAILURE, EXIT_SUCCESS
#include <unistd.h>     // For fork(), execvp(), pid_t, sleep(), usleep(), getpid()
#include <sys/mman.h>   // For shm_open(), mmap(), munmap(), shm_unlink(), PROT_*, MAP_*
#include <sys/stat.h>   // For mode constants (in shm_open/sem_open)
#include <fcntl.h>      // For O_* constants (in shm_open/sem_open)
#include <sys/wait.h>   // For waitpid() (optional but good practice)
#include <semaphore.h>  // For sem_open(), sem_close(), sem_unlink(), sem_t, SEM_FAILED, sem_init()
#include <stdbool.h>    // Make sure bool is available
#include <signal.h>     // For kill(), signals (needed for pid_t and kill, even without sigaction in main)
#include <string.h>     // For memset

// Include custom header files defining shared resources and settings.
#include "dungeon_info.h" // Contains RunDungeon declaration and struct definitions
#include "dungeon_settings.h" // Contains DUNGEON_SIGNAL definition and other game parameters

// Declare the external RunDungeon function from dungeon.o
extern void RunDungeon(pid_t wizard_pid, pid_t rogue_pid, pid_t barbarian_pid);


// --- Function Definitions ---

/*
 * cleanup_resources - Cleans up shared memory and semaphores.
 * Called before exiting the Dungeon Master process.
 * @dungeon_ptr: Pointer to the shared Dungeon struct.
 * @shm_fd: File descriptor for shared memory.
 * @lever1: Pointer to the Lever One semaphore.
 * @lever2: Pointer to the Lever Two semaphore.
 * @barbarian_pid: PID of the Barbarian process.
 * @wizard_pid: PID of the Wizard process.
 * @rogue_pid: PID of the Rogue process.
 */
void cleanup_resources(struct Dungeon *dungeon_ptr, int shm_fd, sem_t *lever1, sem_t *lever2,
                       pid_t barbarian_pid, pid_t wizard_pid, pid_t rogue_pid) {

    printf("[DUNGEON MASTER] Cleaning up resources...\n");

    // Signal characters to exit gracefully if they are still running.
    // They should have SIGINT handlers to catch this.
    if (barbarian_pid > 0) kill(barbarian_pid, SIGINT);
    if (wizard_pid > 0) kill(wizard_pid, SIGINT);
    if (rogue_pid > 0) kill(rogue_pid, SIGINT);

    // Give children a moment to receive the signal and start their cleanup.
    usleep(10000); // Sleep for 10ms.

    // Wait for character processes to terminate.
    // This prevents the parent from cleaning up resources while children might still use them.
    int status;
    if (barbarian_pid > 0) waitpid(barbarian_pid, &status, 0);
    if (wizard_pid > 0) waitpid(wizard_pid, &status, 0);
    if (rogue_pid > 0) waitpid(rogue_pid, &status, 0);
    printf("[DUNGEON MASTER] All characters have exited.\n");

    // Unmap the shared memory segment.
    if (dungeon_ptr != MAP_FAILED && dungeon_ptr != NULL) {
        if (munmap(dungeon_ptr, sizeof(struct Dungeon)) == -1) {
            perror("DUNGEON MASTER: munmap failed");
        }
    }

    // Close the shared memory file descriptor.
    if (shm_fd != -1) {
        if (close(shm_fd) == -1) {
            perror("DUNGEON MASTER: close shm_fd failed");
        }
    }

    // Remove the shared memory object name from the system.
    if (shm_unlink(dungeon_shm_name) == -1) {
        perror("DUNGEON MASTER: shm_unlink failed");
    }

    // Close semaphore descriptors.
    if (lever1 != SEM_FAILED) {
        if (sem_close(lever1) == -1) {
            perror("DUNGEON MASTER: sem_close lever1 failed");
        }
    }
    if (lever2 != SEM_FAILED) {
        if (sem_close(lever2) == -1) {
            perror("DUNGEON MASTER: sem_close lever2 failed");
        }
    }

    // Remove the semaphore names from the system.
    if (sem_unlink(dungeon_lever_one) == -1) {
        perror("DUNGEON MASTER: sem_unlink lever1 failed");
    }
    if (sem_unlink(dungeon_lever_two) == -1) {
        perror("DUNGEON MASTER: sem_unlink lever2 failed");
    }

    printf("[DUNGEON MASTER] Cleanup complete. Exiting.\n");
}

/*
 * error_and_exit - Prints an error message, attempts cleanup, and exits.
 * @msg: The error message to display.
 * Note: This version calls cleanup_resources with the current state of pointers/fds/pids.
 */
void error_and_exit(const char *msg, struct Dungeon *dungeon_ptr, int shm_fd, sem_t *lever1, sem_t *lever2,
                    pid_t barbarian_pid, pid_t wizard_pid, pid_t rogue_pid) {
    perror(msg); // Print the system error message.
    // Call cleanup with the current resource state.
    cleanup_resources(dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    exit(EXIT_FAILURE); // Exit with a failure status.
}


/*
 * main - The main function for the Dungeon Master process.
 * Sets up shared memory and semaphores, forks character processes,
 * calls RunDungeon, and cleans up resources.
 */
int main() {
    printf("[DUNGEON MASTER] Initializing...\n");

    // Declare local variables for resources and PIDs.
    int shm_fd = -1;             // Shared memory file descriptor, initialize to -1.
    struct Dungeon *dungeon_ptr = MAP_FAILED; // Pointer to the Dungeon struct, initialize to MAP_FAILED.
    sem_t *lever1 = SEM_FAILED; // Semaphore for the first lever.
    sem_t *lever2 = SEM_FAILED; // Semaphore for the second lever.

    pid_t barbarian_pid = -1; // PID for the Barbarian process.
    pid_t wizard_pid = -1; // PID for the Wizard process.
    pid_t rogue_pid = -1; // PID for the Rogue process.


    // --- 1. Shared Memory Setup ---
    printf("[DUNGEON MASTER] Creating shared memory...\n");

    // Create or open the shared memory segment.
    // O_CREAT: Create if it doesn't exist.
    // O_RDWR: Open for reading and writing.
    // 0666: Permissions (read/write for owner, group, others).
    shm_fd = shm_open(dungeon_shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        error_and_exit("DUNGEON MASTER: shm_open failed", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    }

    // Set the size of the shared memory segment to the size of the Dungeon struct.
    if (ftruncate(shm_fd, sizeof(struct Dungeon)) == -1) {
        // If ftruncate fails, clean up shared memory and exit.
        error_and_exit("DUNGEON MASTER: ftruncate failed", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    }

    // Map the shared memory segment into this process's address space.
    dungeon_ptr = (struct Dungeon *)mmap(NULL, sizeof(struct Dungeon), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (dungeon_ptr == MAP_FAILED) {
        // If mmap fails, clean up shared memory and exit.
        error_and_exit("DUNGEON MASTER: mmap failed", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    }
    // The file descriptor is typically kept open by the Dungeon Master until munmap/cleanup.

    // Initialize the shared memory structure to zeros.
    memset(dungeon_ptr, 0, sizeof(struct Dungeon));
    dungeon_ptr->running = true; // Set the flag indicating the dungeon is running.
    dungeon_ptr->dungeonPID = getpid(); // Store the Dungeon Master's PID.


    printf("[DUNGEON MASTER] Shared memory created and mapped.\n");

    // --- 2. Semaphore Setup ---
    printf("[DUNGEON MASTER] Creating semaphores...\n");

    // Create or open the first named semaphore for Lever 1.
    // O_CREAT: Create if it doesn't exist.
    // 0666: Permissions.
    // 1: Initial value (binary semaphore, 1 means available).
    lever1 = sem_open(dungeon_lever_one, O_CREAT, 0666, 1);
    if (lever1 == SEM_FAILED) {
        error_and_exit("DUNGEON MASTER: sem_open failed for lever one", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    }

    // Create or open the second named semaphore for Lever 2.
    lever2 = sem_open(dungeon_lever_two, O_CREAT, 0666, 1);
    if (lever2 == SEM_FAILED) {
         error_and_exit("DUNGEON MASTER: sem_open failed for lever two", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    }

    printf("[DUNGEON MASTER] Semaphores created.\n");


    // --- 3. Fork and Exec Character Processes ---
    printf("[DUNGEON MASTER] Spawning characters...\n");

    // Fork and execute the Barbarian process.
    barbarian_pid = fork();
    if (barbarian_pid < 0) {
        error_and_exit("DUNGEON MASTER: Fork failed for Barbarian", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    } else if (barbarian_pid == 0) {
        // Child process: Execute the barbarian program.
        char *barbarian_args[] = {"./barbarian", NULL};
        execvp(barbarian_args[0], barbarian_args);
        // If execvp returns, it failed.
        perror("DUNGEON MASTER: Execvp failed for Barbarian");
        _exit(EXIT_FAILURE); // Use _exit in child after fork.
    }
    printf("[DUNGEON MASTER] Barbarian spawned (PID: %d).\n", barbarian_pid);


    // Fork and execute the Wizard process.
    wizard_pid = fork();
     if (wizard_pid < 0) {
        error_and_exit("DUNGEON MASTER: Fork failed for Wizard", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    } else if (wizard_pid == 0) {
        // Child process: Execute the wizard program.
        char *wizard_args[] = {"./wizard", NULL};
        execvp(wizard_args[0], wizard_args);
        perror("DUNGEON MASTER: Execvp failed for Wizard");
        _exit(EXIT_FAILURE);
    }
    printf("[DUNGEON MASTER] Wizard spawned (PID: %d).\n", wizard_pid);


    // Fork and execute the Rogue process.
    rogue_pid = fork();
     if (rogue_pid < 0) {
        error_and_exit("DUNGEON MASTER: Fork failed for Rogue", dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);
    } else if (rogue_pid == 0) {
        // Child process: Execute the rogue program.
        char *rogue_args[] = {"./rogue", NULL};
        execvp(rogue_args[0], rogue_args);
        perror("DUNGEON MASTER: Execvp failed for Rogue");
        _exit(EXIT_FAILURE);
    }
    printf("[DUNGEON MASTER] Rogue spawned (PID: %d).\n", rogue_pid);

    // Give children a moment to start up and connect to shared resources.
    usleep(100000); // Sleep for 100ms.

    // --- 4. Run the Dungeon Simulation ---
    printf("[DUNGEON MASTER] All characters ready. Starting the dungeon simulation!\n");
    // Call the external RunDungeon function from dungeon.o.
    // This function contains the main game loop and challenge logic.
    RunDungeon(wizard_pid, rogue_pid, barbarian_pid);
    printf("[DUNGEON MASTER] Dungeon simulation finished.\n");

    // --- 5. Cleanup ---
    // Signal children to exit and wait for them, then clean up shared memory and semaphores.
    cleanup_resources(dungeon_ptr, shm_fd, lever1, lever2, barbarian_pid, wizard_pid, rogue_pid);

    return EXIT_SUCCESS; // Indicate successful execution.
}
