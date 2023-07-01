#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main() {
    int pid = fork();
    char a ;
    if(pid == 0){
        printf("this is child\n");
        a = '/';
    }else{
        printf("this is father\n");
        a = '\\';
    }
    for(int i = 0; ; i++){
        if(i % 10000000 == 0){
            write(2, &a, 1);
        }
    }
    exit(0);
    return 0;
}