//
//  main.m
//  testMalloc
//
//  Created by wqan3313 on 2018/11/30.
//
#import <Foundation/Foundation.h>

int main(int argc, const char * argv[]) {
    
#define tinyCount 1008
    static void* memarry[255][tinyCount];
    static void* tinyarry[1000][tinyCount];
    static void* small[tinyCount];

    for (int i = 0; i < 255; i++) {
	for (int j = 0; j < tinyCount; j++) {
	    memarry[i][j] = malloc(i);
	}
	
    }

    for (int i = 0; i < 1000; i++) {
	for (int j = 257; j < 1008; j++) {
	    tinyarry[i][j-257] = malloc(j);
	    if (i % 2 == 1) {
		//free(tinyarry[i][j-257]);
	    }
	}
	
    }
    
    for (int i = 128*1024; i > 128 ; i--) {
	for (int j = 0; j < 20; j++) {
	    void *large = malloc(i * 1024);
	    if (i % 2 == 0) {
		free(large);
	    }
	}
    }

    while (1) {
	sleep(1);
    }
    return 0;
}
