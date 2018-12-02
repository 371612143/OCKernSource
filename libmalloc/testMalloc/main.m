//
//  main.m
//  testMalloc
//
//  Created by wqan3313 on 2018/11/30.
//


int main(int argc, const char * argv[]) {

    void* memarry[255][1000];
    unsigned int bytes = 255;
    
    for (int i = 0; i < 255; i++) {
	for (int j = 0; j < 1000; j++)
	memarry[i][j] = malloc(i);
	//free(memarry[i]);
    }
    for (int i = 0; i < 100000; i++) {
	//free(memarry[i]);
    }
    for (int i = 257; i < 1008; i++) {
	void *tinyMalloc = malloc(512);
    }
    
    void *smallMalloc = malloc(1024);
    void *largerMalloc = malloc(127*1024);
    NSLog(@"Hello, World!");

    return 0;
}
