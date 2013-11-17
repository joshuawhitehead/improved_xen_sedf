//
//  main.c
//  stubdom
//
//  CIS 467

#include <stdio.h>
#include <math.h>


/* Constant */
#define BUFFER_LENGTH 100
#define WORKLOAD_SIZE 10000000


void workloadProgram1();
void workloadProgram2();


void clearBuffer(void) {
    char inputBuffer[BUFFER_LENGTH];
    if (fgets(inputBuffer, BUFFER_LENGTH, stdin) == NULL) {
        (void)printf("Unlösbares Eingabe-Problem");
        (void)exit(-1);
    }
}



int main(int argc, const char * argv[])
{
    int userInput;
    while (1) {
        userInput = 0;
        (void)printf("\nPlease select a program to run.\n'1'\t-\tDefault\n");
        (void)scanf("%d", &userInput);
        (void)clearBuffer();
        
        (void)sleep(1);
        
        (void)printf("Input: %d\n", userInput);
        
        switch (userInput) {
            case 1:
                (void)printf("Running program 1\n");
                workloadProgram1();
                break;
            case 0:
            default:
                (void)printf("Invalid program\n");
                workloadProgram2();
                break;
        }        
    }
    
    
    return 0;
}



/** test workload example **/
void workloadProgram1() {
    int i,j;
    int sum = 0;
    for (i = 0; i < WORKLOAD_SIZE; i++) {
        // do some work
        sum += i + pow(0.123456, sqrt(sum*sum));
        
        // printf slows down the loop
        printf("%d\t",i);
    }
    printf("\n---end\n\n");
    return;
}

/** TODO: create a workload according to documentation **/
void workloadProgram2() {
    // TODO:
    return;
}

