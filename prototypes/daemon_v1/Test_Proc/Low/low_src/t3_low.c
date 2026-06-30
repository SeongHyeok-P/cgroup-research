#include<stdio.h>
#define MAX_TIME 8e9
int main(void){
        printf("This is Low test 3\n");
        long a = 0;
        for(long i = 0; i < MAX_TIME; i++) {
                a += 1;
        }
        printf("Low test 3: %ld\n",a);
        return 0;
}
