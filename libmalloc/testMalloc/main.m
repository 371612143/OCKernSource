//
//  main.m
//  testMalloc
//
//  Created by wqan3313 on 2018/11/30.
//


int main(int argc, const char * argv[]) {
    
#define tinyCount 2000
    void* memarry[255][tinyCount];
    void* small[tinyCount];
    for (int i = 0; i < 255; i++) {
	for (int j = 0; j < tinyCount; j++) {
	    memarry[i][j] = malloc(255);
	}
	
    }
 
    for (int i = 257; i < 1008; i++) {
	small[i-257] = malloc(i);
    }
    for (int i = 1009; i < 127*1024; i++) {
	void *smallMalloc = malloc(i);
    }

    while (1) {
	
    }
    return 0;
}
