#include <stdio.h>
#include <sys/syscall.h>

#define SLOB_REC 317
#define SLOB_FREE 318

int main(int argc, char *argv[])
{
	int v1, v2;

	v1 = syscall(SLOB_REC);
	v2 = syscall(SLOB_FREE);
	printf ("SLOB memory reserved: %x fragmentation %.2f%%\n", v1, 100*(float)v2/(float)v1);
}
