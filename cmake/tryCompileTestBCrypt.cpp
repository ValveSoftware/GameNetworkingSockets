#include <Windows.h>
#include <bcrypt.h>
#include <cstdio>
#pragma comment(lib, "bcrypt.lib")

int main(int, char **)
{
	printf("%p\n", &BCryptEncrypt);
}
