int a[512] __attribute__((aligned(0x20)));
int b[512] __attribute__((aligned(0x20)));
int c[512] __attribute__((aligned(0x20)));

int main () {
  for (int i=0; i<512; i++){
    a[i] = b[i] + c[i];
  }
}
