//
//  cpu_test.m
//  kobjc
//
//  Created by DaeHoon Zee on 2020/06/19.
//  Copyright © 2020 DaeHoon Zee. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

const static int CNT_LOOP = 1000*1000*100;

typedef struct {
    int refCount;
    int test;
} Foo;
int outer_refCount = 0;

void test0() {
    int cnt = 0;
    CFTimeInterval t1 = CACurrentMediaTime();
    Foo* foo = malloc(sizeof(Foo));
    foo->refCount = 0;
    foo->test = 1;
    for (int i = 0; i < CNT_LOOP; i ++) {
        cnt += foo->test;
    }
    CFTimeInterval t2 = CACurrentMediaTime();
    NSLog(@"%f, %d, %d", t2 - t1, cnt, foo->refCount);
}

void test_refCount(int* pRefCount, Foo* foo) {
    CFTimeInterval t0 = CACurrentMediaTime();
    int cnt = 0;
    for (int i = 0; i < CNT_LOOP; i ++) {
        __sync_add_and_fetch(&foo->refCount, 1);
        cnt += foo->test;
    }
    CFTimeInterval t1 = CACurrentMediaTime();
    NSLog(@"elapsed time %f", t1 - t0);
}

void test1() {
    Foo* foo = malloc(sizeof(Foo));
    foo->refCount = 0;
    foo->test = 1;
    int stack_refCount = 0;
    test_refCount(&foo->refCount, foo);
    test_refCount(&outer_refCount, foo);
    test_refCount(&stack_refCount, foo);
}

void cpu_cache_test() {
    test0();
    test1();
}
