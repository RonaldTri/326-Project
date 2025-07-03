/*
 * barbarian.c - This process represents the Barbarian character.
 * It connects to shared memory and semaphores to interact with the Dungeon Master.
 */

// Include necessary headers for system calls and standard libraries.
#define _XOPEN_SOURCE 700       // Ensure POSIX features are available
#define _POSIX_C_SOURCE 200809L // Ensure POSIX.1-2008 compliance

#include <stdio.h>      // For printf, perror
#include <stdlib.h>     // For exit
#include <unistd.h>     // For fork, usleep, getpid
#include <sys/mman.h>   // For shared memory functions (shm_open, mmap, munmap)
#include <sys/stat.h>   // For mode constants
#include <fcntl.h>      // For file control options
#include <signal.h>     // For signal handling (sigaction, sigsuspend)
#include <semaphore.h>  // For semaphore functions (sem_open, sem_close, sem_wait, sem_post)
#include <stdbool.h>    // For bool type
#include <string.h>     // For memset
#include <time.h>       // For timing (optional)

// Include custom header files for shared resources and settings.
#include "dungeon_info.h"   // Defines shared memory/semaphore names and struct layout
#include "dungeon_settings.h" // Defines signals and game parameters

// --- Global Variables ---
// Pointers to shared resources accessed by the main loop and signal handlers.
struct Dungeon *dungeon_ptr = NULL; // Pointer to the shared Dungeon struct
sem_t *lever1_sem = SEM_FAILED;     // Pointer to the Lever One semaphore
sem_t *lever2_sem = SEM_FAILED;     // Pointer to the Lever Two semaphore
int shm_fd = -1;                    // File descriptor for shared memory

// Flag to control the main loop's exit, set by the SIGINT handler.
volatile sig_atomic_t exit_flag = 0;

// --- Function Definitions ---

/*
 * error_exit - Prints an error message and cleans up resources before exiting.
 * @msg: The error message to display.
 */
void error_exit(const char *msg) {
    perror(msg); // Print the system error message.
    // Attempt to unmap shared memory if it was mapped.
    if (dungeon_ptr != MAP_FAILED && dungeon_ptr != NULL) {
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        dungeon_ptr = NULL;
    }
    // Close the shared memory file descriptor if it was opened.
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }
    // Close semaphore descriptors if they were opened.
    if (lever1_sem != SEM_FAILED) {
        sem_close(lever1_sem);
        lever1_sem = SEM_FAILED;
    }
     if (lever2_sem != SEM_FAILED) {
        sem_close(lever2_sem);
        lever2_sem = SEM_FAILED;
    }
    exit(EXIT_FAILURE); // Exit with a failure status.
}

/*
 * sigint_handler - Handles the SIGINT signal (Ctrl+C) for graceful exit.
 * Sets the global exit_flag to terminate the main loop.
 * @signum: The signal number (SIGINT).
 */
void sigint_handler(int signum) {
    exit_flag = 1; // Indicate that the process should exit.
}

/*
 * barbarian_signal_handler - Handles signals from the Dungeon Master.
 * Responds to DUNGEON_SIGNAL for monster attacks and SEMAPHORE_SIGNAL for the treasure room.
 * @signum: The signal number received.
 */
void barbarian_signal_handler(int signum) {
    // Return immediately if the exit flag is set.
    if (exit_flag) {
        return;
    }

    // Return if shared memory is not valid or the dungeon is not running.
    if (dungeon_ptr == NULL || dungeon_ptr == MAP_FAILED || !dungeon_ptr->running) {
        return;
    }

    // Handle the DUNGEON_SIGNAL for monster encounters.
    if (signum == DUNGEON_SIGNAL) {
        // Copy the monster's health to the barbarian's attack field in shared memory.
        dungeon_ptr->barbarian.attack = dungeon_ptr->enemy.health;

        // Yield briefly to allow the Dungeon Master to read the updated value.
        usleep(100);

    }
    // Handle the SEMAPHORE_SIGNAL for the treasure room challenge.
    else if (signum == SEMAPHORE_SIGNAL) {
        printf("[BARBARIAN %d] Received SEMAPHORE_SIGNAL. Attempting to hold a lever...\n", getpid());

        // Attempt to acquire Lever 1 using sem_wait(), which blocks if the semaphore is zero.
        if (sem_wait(lever1_sem) == 0) {
            printf("[BARBARIAN %d] Successfully grabbed Lever 1 (sem_wait). Holding...\n", getpid());

            // Wait in a loop until the Rogue collects the treasure (indicated by spoils[3] != '\0').
            while (dungeon_ptr->running && (dungeon_ptr->spoils[3] == '\0') && exit_flag == 0) {
                 // Sleep to avoid busy-waiting while holding the semaphore.
                 usleep(100000);
            }

            // Release Lever 1 by posting to the semaphore when the Rogue is done or the dungeon ends.
            if (sem_post(lever1_sem) == 0) {
                printf("[BARBARIAN %d] Rogue collected spoils or dungeon finished. Released Lever 1 (sem_post).\n", getpid());
            } else {
                perror("BARBARIAN: sem_post failed for lever 1");
            }

        }
        // If Lever 1 acquisition failed, indicate that another character likely got it.
        else {
             printf("[BARBARIAN %d] Did not grab Lever 1. Another character likely got it.\n", getpid());
             usleep(100); // Yield briefly.
        }

    }
    // Handle any unexpected signals.
    else {
        usleep(100); // Yield briefly.
    }
}


/*
 * main - The main function for the Barbarian process.
 * Initializes connections to shared memory and semaphores, sets up signal handlers,
 * and enters a loop to wait for signals from the Dungeon Master.
 */
int main() {
    printf("[BARBARIAN] Process started. PID: %d\n", getpid());

    // --- 1. Connect to Shared Memory ---
    // Open the shared memory object for read/write access.
    shm_fd = shm_open(dungeon_shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        error_exit("BARBARIAN: shm_open failed");
    }

    // Map the shared memory object into the process's address space.
    dungeon_ptr = (struct Dungeon *)mmap(NULL, sizeof(struct Dungeon), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (dungeon_ptr == MAP_FAILED) {
        close(shm_fd); shm_fd = -1;
        error_exit("BARBARIAN: mmap failed");
    }
    // Close the file descriptor as it's no longer needed after mapping.
    close(shm_fd); shm_fd = -1;
    printf("[BARBARIAN] Connected to shared memory.\n");

    // --- 2. Connect to Semaphores ---
    // Open the named semaphores for Lever One and Lever Two.
    lever1_sem = sem_open(dungeon_lever_one, O_RDWR);
    if (lever1_sem == SEM_FAILED) {
        munmap(dungeon_ptr, sizeof(struct Dungeon)); dungeon_ptr = NULL;
        error_exit("BARBARIAN: sem_open failed for lever one");
    }

    lever2_sem = sem_open(dungeon_lever_two, O_RDWR);
    if (lever2_sem == SEM_FAILED) {
        sem_close(lever1_sem); lever1_sem = SEM_FAILED;
        munmap(dungeon_ptr, sizeof(struct Dungeon)); dungeon_ptr = NULL;
        error_exit("BARBARIAN: sem_open failed for lever two");
    }
    printf("[BARBARIAN] Connected to semaphores.\n");

    // --- 3. Set up Signal Handlers ---
    struct sigaction sa_dungeon, sa_semaphore, sa_sigint;

    // Configure and register the handler for DUNGEON_SIGNAL.
    memset(&sa_dungeon, 0, sizeof(sa_dungeon));
    sa_dungeon.sa_handler = barbarian_signal_handler;
    sa_dungeon.sa_flags = 0;
    if (sigaction(DUNGEON_SIGNAL, &sa_dungeon, NULL) == -1) {
        sem_close(lever1_sem); sem_close(lever2_sem);
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        error_exit("BARBARIAN: sigaction failed for DUNGEON_SIGNAL");
    }
    printf("[BARBARIAN] Signal handler set up for DUNGEON_SIGNAL (%d).\n", DUNGEON_SIGNAL);

    // Configure and register the handler for SEMAPHORE_SIGNAL.
    memset(&sa_semaphore, 0, sizeof(sa_semaphore));
    sa_semaphore.sa_handler = barbarian_signal_handler;
    sa_semaphore.sa_flags = 0;
    if (sigaction(SEMAPHORE_SIGNAL, &sa_semaphore, NULL) == -1) {
        sem_close(lever1_sem); sem_close(lever2_sem);
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        error_exit("BARBARIAN: sigaction failed for SEMAPHORE_SIGNAL");
    }
    printf("[BARBARIAN] Signal handler set up for SEMAPHORE_SIGNAL (%d).\n", SEMAPHORE_SIGNAL);

    // Configure and register the handler for SIGINT (Ctrl+C).
    memset(&sa_sigint, 0, sizeof(sa_sigint));
    sa_sigint.sa_handler = sigint_handler;
    sa_sigint.sa_flags = 0;
    if (sigaction(SIGINT, &sa_sigint, NULL) == -1) {
         perror("BARBARIAN: sigaction failed for SIGINT");
    }
    printf("[BARBARIAN] Signal handler set up for SIGINT.\n");

    // --- 4. Main Loop: Wait for Signals ---
    printf("[BARBARIAN] Ready to receive signals...\n");

    // Prepare a signal mask to block all signals except the ones we handle.
    sigset_t mask;
    sigfillset(&mask); // Block all signals initially.
    sigdelset(&mask, DUNGEON_SIGNAL); // Unblock DUNGEON_SIGNAL.
    sigdelset(&mask, SEMAPHORE_SIGNAL); // Unblock SEMAPHORE_SIGNAL.
    sigdelset(&mask, SIGINT);       // Unblock SIGINT.

    // Use sigsuspend to atomically release the current mask and wait for a signal.
    while (dungeon_ptr != NULL && dungeon_ptr != MAP_FAILED && dungeon_ptr->running && exit_flag == 0) {
        sigsuspend(&mask);

        // Yield briefly after a signal handler returns if the loop continues.
        if (dungeon_ptr != NULL && dungeon_ptr != MAP_FAILED && dungeon_ptr->running && exit_flag == 0) {
             usleep(100);
        }
    }

    printf("[BARBARIAN] Dungeon simulation finished or interrupted. Exiting.\n");

    // --- 5. Cleanup Resources ---
    // Unmap shared memory.
    if (dungeon_ptr != MAP_FAILED && dungeon_ptr != NULL) {
        if (munmap(dungeon_ptr, sizeof(struct Dungeon)) == -1) {
            perror("BARBARIAN: munmap failed");
        }
        dungeon_ptr = NULL;
    }

    // Close semaphore descriptors.
    if (lever1_sem != SEM_FAILED) {
        if (sem_close(lever1_sem) == -1) {
            perror("BARBARIAN: sem_close lever1 failed");
        }
        lever1_sem = SEM_FAILED;
    }
     if (lever2_sem != SEM_FAILED) {
        if (sem_close(lever2_sem) == -1) {
            perror("BARBARIAN: sem_close lever2 failed");
        }
        lever2_sem = SEM_FAILED;
    }

    printf("[BARBARIAN] Cleanup complete. Exiting.\n");

    return EXIT_SUCCESS;
}
