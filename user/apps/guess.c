/* guess: a number guessing game that reads from the terminal. */
#include <zenith.h>

int main(void)
{
    int secret = (int)(random() % 100) + 1, tries = 0;
    char line[64];
    printf("I am thinking of a number between 1 and 100.\n");
    for (;;) {
        printf("Your guess: ");
        if (!readline(line, sizeof(line))) return 0;
        int g = atoi(line);
        tries++;
        if (g < secret) printf("\x1b[93mHigher!\x1b[0m\n");
        else if (g > secret) printf("\x1b[93mLower!\x1b[0m\n");
        else {
            printf("\x1b[92mCorrect! You needed %d tries.\x1b[0m\n", tries);
            return 0;
        }
    }
}
