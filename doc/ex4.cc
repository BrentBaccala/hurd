#include <thread>

int a[512];
int b[512];
int c[512];

int operation(int * ap, int * bp, int * cp, int count)
{
  for (int i = 0; i < count; i++) {
    cp[i] = ap[i] + bp[i];
  }
}

const int num_threads = 3;

int main () {
  int start_index[num_threads+1];
  std::thread thread[num_threads];

  start_index[0] = 0;
  for (int i=1; i<num_threads; i++) {
    start_index[i] = i*(512/num_threads) & ~15;
  }
  start_index[num_threads] = 512;

  for (int i=0; i<num_threads; i++) {
    thread[i] = std::thread(operation,
			    &a[start_index[i]], &b[start_index[i]], &c[start_index[i]],
			    start_index[i+1] - start_index[i]);
  }

  for (int i=0; i<num_threads; i++) {
    thread[i].join();
  }
}
