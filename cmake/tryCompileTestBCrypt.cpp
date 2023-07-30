#include <Windows.h>
#include <bcrypt.h>
#include <cstdio>

int main(int, char **)
{
	printf("%p\n", &BCryptEncrypt);
}
