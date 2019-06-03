int a[512] __attribute__((aligned(64)));
int b[512] __attribute__((aligned(64)));
int c[512] __attribute__((aligned(64)));

int main () {
  for (int i=0; i<512; i++){
    a[i] = b[i] + c[i];
  }
}
