/*
 * rogue.c - This process represents the Rogue character.
 * It connects to shared memory and semaphores to interact with the Dungeon Master.
 * The Rogue attempts to disarm traps using a binary search approach and collects
 * treasure from the treasure room after the Barbarian and Wizard hold the levers.
 */

// Include necessary headers for system calls and standard libraries.
#define _XOPEN_SOURCE 700       // Ensure POSIX features are available
#define _POSIX_C_SOURCE 200809L // Ensure POSIX.1-2008 compliance

#include <stdio.h>      // For printf, perror
#include <stdlib.h>     // For exit
#include <unistd.h>     // For fork, usleep, sleep, getpid, pause
#include <sys/mman.h>   // For shared memory functions (shm_open, mmap, munmap)
#include <sys/stat.h>   // For mode constants
#include <fcntl.h>      // For file control options
#include <signal.h>     // For signal handling (sigaction, sigsuspend)
#include <semaphore.h>  // For semaphore functions (sem_open, sem_close, sem_wait, sem_post)
#include <stdbool.h>    // For bool type
#include <string.h>     // For memset
#include <math.h>       // For binary search calculations (midpoint, fabs)
#include <time.h>       // For time(), difftime()



// Include custom header files for shared resources and settings.
#include "dungeon_info.h"   // Defines shared memory/semaphore names and struct layout
#include "dungeon_settings.h" // Defines signals, MAX_PICK_ANGLE, and other game parameters

// --- Global Variables ---
// Pointers to shared resources accessed by the main loop and signal handlers.
struct Dungeon *dungeon_ptr = NULL; // Pointer to the shared Dungeon struct
sem_t *lever1_sem = SEM_FAILED;     // Pointer to the Lever One semaphore
sem_t *lever2_sem = SEM_FAILED;     // Pointer to the Lever Two semaphore
int shm_fd = -1;                    // File descriptor for shared memory (used only during setup)

// Flag to control the main loop's exit, set by the SIGINT handler.
volatile sig_atomic_t exit_flag = 0;

// --- Function Definitions ---

/*
 * error_exit - Prints an error message and attempts to clean up resources before exiting.
 * @msg: The error message to display.
 */
void error_exit(const char *msg) {
    perror(msg); // Print the system error message.
    // Attempt to unmap shared memory if it was mapped.
    if (dungeon_ptr != MAP_FAILED && dungeon_ptr != NULL) {
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        dungeon_ptr = NULL;
    }
    // Close the shared memory file descriptor if it was opened (unlikely at error stage).
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
 * NOTE: The main signal handler now also catches SIGINT.
 * @signum: The signal number (SIGINT).
 */
void sigint_handler(int signum) {
    // Setting the flag is the primary action.
    // The main handler can also check signum == SIGINT.
    exit_flag = 1;
}

/*
 * rogue_signal_handler - Handles signals from the Dungeon Master (DUNGEON_SIGNAL,
 * SEMAPHORE_SIGNAL) and SIGINT.
 * For traps (DUNGEON_SIGNAL), it now enters an internal loop to complete the search.
 * @signum: The signal number received.
 */
void rogue_signal_handler(int signum) {
    // --- Static variables to maintain binary search state across signals ---
    // These persist between calls to the handler for DIFFERENT traps.
    static float current_low = 0.0;
    static float current_high = MAX_PICK_ANGLE;
    

    if (signum == SIGINT) {
       exit_flag = 1;
       return; // Just set flag and return
    }

    if (exit_flag) return; // Check flag again after potential SIGINT
    if (dungeon_ptr == NULL || dungeon_ptr == MAP_FAILED || !dungeon_ptr->running) return;


    if (signum == DUNGEON_SIGNAL) {

        // Check trap state *once* when signal arrives
        if (dungeon_ptr->trap.locked) {

            // --- Reset bounds logic (Attempt 3 approach) ---
            // Reset bounds only if the trap state indicates a new search is needed.
            // Infer this if the direction isn't suggesting an ongoing search ('u'/'d')
            // or completion ('-').
            char initial_direction = dungeon_ptr->trap.direction;
            float initial_pick = dungeon_ptr->rogue.pick; // Read initial pick too

            // Reset if direction implies start ('w', 't', '\0') OR if pick is the initial 50.0?
            // Let's reset if direction is NOT 'u', 'd', or '-'
            if (initial_direction != 'u' && initial_direction != 'd' && initial_direction != '-') {

                 current_low = 0.0;
                 current_high = MAX_PICK_ANGLE;
                 
            }
            // --- End Reset Bounds Logic ---


            // --- Internal loop to perform binary search ---
            time_t loop_start_time = time(NULL);
            while (dungeon_ptr->trap.locked && dungeon_ptr->running && exit_flag == 0) {

                // Check for timeout
                if (difftime(time(NULL), loop_start_time) > (SECONDS_TO_PICK - 0.5)) {

                     break; // Exit internal loop
                }

                // Read current feedback and pick value *inside the loop*
                char current_direction = dungeon_ptr->trap.direction;
                float current_pick = dungeon_ptr->rogue.pick; // Read most recent pick


                if (current_direction == '-') {

                     // Bounds will be reset below, outside the loop, if trap becomes unlocked
                     break; // Exit internal loop
                } else if (current_direction == 'u' || current_direction == 'd') {
                     // --- Valid feedback, update bounds ---
                     // Use the 'current_pick' read *in this loop iteration* which
                     // represents the pick the dungeon gave feedback on.
                     if (current_direction == 'u') { // Pick was too low
                         if (current_pick > current_low) current_low = current_pick;

                     } else { // Pick was too high
                         if (current_pick < current_high) current_high = current_pick;

                     }

                     // --- Calculate and write next pick if bounds valid ---
                    if (current_high > current_low && (current_high - current_low) > 0.000001) {
                        float next_pick = current_low + (current_high - current_low) / 2.0;

                        // --- Write to Shared Memory ---
                        dungeon_ptr->rogue.pick = next_pick;
                        dungeon_ptr->trap.direction = 't'; // Signal guess made
                        
                    } else {

                         break; // Exit internal loop
                    }
                }
            } // --- End internal while loop ---

            // --- After internal loop ---
            if (!dungeon_ptr->trap.locked) {

                current_low = 0.0; // Reset state for the *next* trap
                current_high = MAX_PICK_ANGLE;
            } 
            
        } else { // Trap not locked when signal arrived

             current_low = 0.0; // Reset state for the *next* trap
             current_high = MAX_PICK_ANGLE;
        }

        return; // Exit signal handler

    } // End DUNGEON_SIGNAL handling

    // --- SEMAPHORE_SIGNAL handling ---
    else if (signum == SEMAPHORE_SIGNAL) {
        // --- Rogue Logic: Treasure Room ---
        printf("[ROGUE %d] Received SEMAPHORE_SIGNAL. Entering treasure room...\n", getpid());

        int spoils_count = 0;
        memset(dungeon_ptr->spoils, '\0', sizeof(dungeon_ptr->spoils)); // Ensure null termination

        // Loop while dungeon running, haven't exited, and haven't collected all 4 chars
        time_t treasure_start_time = time(NULL); // Timeout for treasure
        while (dungeon_ptr != NULL && dungeon_ptr != MAP_FAILED && dungeon_ptr->running && exit_flag == 0 && spoils_count < 4) {

            // Check for treasure timeout
            if (difftime(time(NULL), treasure_start_time) > TIME_TREASURE_AVAILABLE) {
                 printf("[ROGUE %d] Treasure collection timed out!\n", getpid());
                 break;
            }

            // Check the specific index in 'treasure' corresponding to the *next* spoil needed
            if (dungeon_ptr->treasure[spoils_count] != '\0') {
                // Copy the character
                dungeon_ptr->spoils[spoils_count] = dungeon_ptr->treasure[spoils_count];
                printf("[ROGUE %d] Collected treasure character %d: '%c'\n",
                       getpid(), spoils_count + 1, dungeon_ptr->spoils[spoils_count]);
                spoils_count++;
            }
        } // End while collecting spoils

        // Check if loop exited because all spoils collected
        if (spoils_count == 4) {
             printf("[ROGUE %d] All spoils collected: '%s'.\n", getpid(), dungeon_ptr->spoils);
             // The Barbarian/Wizard should see spoils[3] != '\0' and release levers.
        } else {
             printf("[ROGUE %d] Exited treasure collection early (count=%d, running=%d, exit_flag=%d).\n",
                   getpid(), spoils_count,
                   (dungeon_ptr ? dungeon_ptr->running : -1), // Check pointer before deref
                   exit_flag);
        }

        return; // Exit semaphore handler
    } // End of SEMAPHORE_SIGNAL handling

} // --- End of rogue_signal_handler ---


/*
 * main - The main function for the Rogue process.
 * Initializes connections to shared memory and semaphores, sets the initial pick,
 * sets up signal handlers, and enters a loop to wait for signals using pause().
 */
int main() {
    printf("[ROGUE] Process started. PID: %d\n", getpid());

    // --- 1. Connect to Shared Memory ---
    shm_fd = shm_open(dungeon_shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        error_exit("ROGUE: shm_open failed");
    }

    dungeon_ptr = (struct Dungeon *)mmap(NULL, sizeof(struct Dungeon), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (dungeon_ptr == MAP_FAILED) {
        close(shm_fd); // Close fd before error_exit if open
        error_exit("ROGUE: mmap failed");
    }
    // Close the file descriptor as it's no longer needed after mapping.
    close(shm_fd);
    // shm_fd = -1; // Mark as closed (optional)
    printf("[ROGUE] Connected to shared memory.\n");

    // --- Set Initial Rogue Pick and Direction ---
    // Do this *after* mapping shared memory.
    dungeon_ptr->rogue.pick = MAX_PICK_ANGLE / 2.0;
    dungeon_ptr->trap.direction = 't'; // Signal initial pick is ready
    printf("[ROGUE] Set initial pick to %.6f and direction to 't'.\n", dungeon_ptr->rogue.pick);


    // --- 2. Connect to Semaphores ---
    lever1_sem = sem_open(dungeon_lever_one, O_RDWR);
    if (lever1_sem == SEM_FAILED) {
        munmap(dungeon_ptr, sizeof(struct Dungeon)); dungeon_ptr = NULL;
        error_exit("ROGUE: sem_open failed for lever one");
    }

    lever2_sem = sem_open(dungeon_lever_two, O_RDWR);
    if (lever2_sem == SEM_FAILED) {
        sem_close(lever1_sem); lever1_sem = SEM_FAILED; // Clean up first semaphore
        munmap(dungeon_ptr, sizeof(struct Dungeon)); dungeon_ptr = NULL;
        error_exit("ROGUE: sem_open failed for lever two");
    }
    printf("[ROGUE] Connected to semaphores.\n");

    // --- 3. Set up Signal Handlers ---
    struct sigaction sa; // Use one struct, reset for each signal

    // Configure and register the handler for DUNGEON_SIGNAL.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = rogue_signal_handler;
    sa.sa_flags = 0; // No SA_RESTART needed with pause() loop usually
    sigemptyset(&sa.sa_mask); // Ensure mask is clear initially
    if (sigaction(DUNGEON_SIGNAL, &sa, NULL) == -1) {
        // Attempt cleanup before exiting
        sem_close(lever1_sem); sem_close(lever2_sem);
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        error_exit("ROGUE: sigaction failed for DUNGEON_SIGNAL");
    }
    printf("[ROGUE] Signal handler set up for DUNGEON_SIGNAL (%d).\n", DUNGEON_SIGNAL);

    // Configure and register the handler for SEMAPHORE_SIGNAL.
    memset(&sa, 0, sizeof(sa)); // Reset struct
    sa.sa_handler = rogue_signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SEMAPHORE_SIGNAL, &sa, NULL) == -1) {
        sem_close(lever1_sem); sem_close(lever2_sem);
        munmap(dungeon_ptr, sizeof(struct Dungeon));
        error_exit("ROGUE: sigaction failed for SEMAPHORE_SIGNAL");
    }
    printf("[ROGUE] Signal handler set up for SEMAPHORE_SIGNAL (%d).\n", SEMAPHORE_SIGNAL);

    // Configure and register the handler for SIGINT (Ctrl+C).
    // Using the main handler now, but could use the separate one too.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = rogue_signal_handler; // Route SIGINT to main handler
    // sa.sa_handler = sigint_handler; // Or use the dedicated simpler handler
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
         perror("ROGUE: sigaction failed for SIGINT");
         // Continue running even if SIGINT handler fails? Or exit? Let's continue.
    }
    printf("[ROGUE] Signal handler set up for SIGINT.\n");

    // --- 4. Main Loop: Wait for Signals ---
    printf("[ROGUE] Ready to receive signals...\n");

    // Loop while the dungeon is running and exit flag is not set
    while (dungeon_ptr != NULL && dungeon_ptr != MAP_FAILED && dungeon_ptr->running && exit_flag == 0) {
        pause(); // Wait for any handled signal to arrive
        // When a signal arrives, its handler will run, then pause() will return, and the loop continues.
    }


    printf("[ROGUE] Dungeon simulation finished or interrupted. Exiting.\n");

    // --- 5. Cleanup Resources ---
    // Unmap shared memory.
    if (dungeon_ptr != MAP_FAILED && dungeon_ptr != NULL) {
        if (munmap(dungeon_ptr, sizeof(struct Dungeon)) == -1) {
            perror("ROGUE: munmap failed");
        }
        dungeon_ptr = NULL;
    }

    // Close semaphore descriptors.
    if (lever1_sem != SEM_FAILED) {
        if (sem_close(lever1_sem) == -1) {
            perror("ROGUE: sem_close lever1 failed");
        }
        lever1_sem = SEM_FAILED;
    }
     if (lever2_sem != SEM_FAILED) {
        if (sem_close(lever2_sem) == -1) {
            perror("ROGUE: sem_close lever2 failed");
        }
        lever2_sem = SEM_FAILED;
    }

    printf("[ROGUE] Cleanup complete. Exiting.\n");

    return EXIT_SUCCESS;
}
