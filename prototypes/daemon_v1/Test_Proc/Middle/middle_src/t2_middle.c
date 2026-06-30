#include<stdio.h>
#define MAX_TIME 8e9
int main(void){
        printf("This is Middle test 2\n");
        long a = 0;
        for(long i = 0; i < MAX_TIME; i++) {
                a += 1;
        }
        printf("Middle test 2: %ld\n",a);
        return 0;
}
