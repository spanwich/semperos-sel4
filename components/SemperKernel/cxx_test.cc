/*
 * cxx_test.cc -- Verify C++ new/delete/virtual dispatch in CAmkES
 */

extern "C" {
#include <stdio.h>
}

class Base {
public:
    virtual int value() = 0;
    virtual ~Base() {}
};

class Derived : public Base {
    int v;
public:
    Derived(int v) : v(v) {}
    int value() override { return v; }
};

extern "C" void cxx_test(void) {
    printf("[CXX] Testing new/delete/virtual dispatch...\n");

    Base* obj = new Derived(42);
    if (!obj) {
        printf("[CXX] FAILED: new returned nullptr\n");
        return;
    }

    int result = obj->value();
    if (result != 42) {
        printf("[CXX] FAILED: virtual dispatch returned %d, expected 42\n", result);
        delete obj;
        return;
    }

    delete obj;
    printf("[CXX] OK: new/delete/virtual dispatch all working (got %d)\n", result);
}
