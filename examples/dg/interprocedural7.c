#include <assert.h>

void foo(int *a)
{
	++a;
	*a = 8;
}

int main(int argc, char *argv[], char *envp[])
{
	int a[2] = {0,1};
	foo(a);
	assert(a[1] == 8);
	return 0;
}
