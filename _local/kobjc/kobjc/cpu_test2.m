//
//  cpu_test2.m
//  kobjc
//
//  Created by DaeHoon Zee on 2020/11/17.
//  Copyright © 2020 DaeHoon Zee. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

#define USE_MALLOC 1
const int MAX_OBJ_CNT = 1000 * 1000;
typedef struct { int values[1024]; } Foo;
Foo* memPool;
Foo* freeItems[MAX_OBJ_CNT];
int cntFree = MAX_OBJ_CNT;

void initMemPool() {
    memPool = (Foo*)malloc(sizeof(Foo) * MAX_OBJ_CNT);
    for (int i = 0; i < MAX_OBJ_CNT; i++) {
        freeItems[i] = memPool + i;
    }
}

static int cntMark = 0;
void markFoo(Foo* foo) { cntMark += foo->values[0]; }
Foo* allocFoo() {
    return USE_MALLOC ? (Foo*)malloc(sizeof(Foo)) : freeItems[--cntFree];
}
void freeFoo(Foo* foo) {
    if (USE_MALLOC) free(foo); else freeItems[cntFree++] = foo;
}

void testA() {
    CFTimeInterval t1 = CACurrentMediaTime();
    for (int i = 0; i < MAX_OBJ_CNT; i ++) {
        Foo* foo = allocFoo();
        markFoo(foo);
        freeFoo(foo);
    }
    CFTimeInterval t2 = CACurrentMediaTime();
    NSLog(@"testA %f\n", t2 - t1);
}

void testB() {
    Foo** items = (Foo*)malloc(sizeof(Foo*) * MAX_OBJ_CNT);
    CFTimeInterval t1 = CACurrentMediaTime();
    for (int i = 0; i < MAX_OBJ_CNT; i ++) {
        items[i] = allocFoo();
    }
    for (int i = 0; i < MAX_OBJ_CNT; i ++) {
        Foo* foo = items[i];
        assert(foo != NULL);
        markFoo(foo);
        freeFoo(foo);
    }
    CFTimeInterval t2 = CACurrentMediaTime();
    NSLog(@"testB %f\n", t2 - t1);
}

void lzay_delloc_test() {
    initMemPool();
    testA();
    testB();
    testB();
    NSLog(@"");
}
