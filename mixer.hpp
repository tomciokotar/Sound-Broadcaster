#ifndef MIXER_HPP
#define MIXER_HPP

#include <cstdio>
#include <climits>

using std::size_t;

struct mixer_input
{
	void* data;
	size_t len;
	size_t consumed;
};

void mixer(struct mixer_input* inputs, size_t n, void* output_buf,
		   size_t* output_size, unsigned long tx_interval_ms)
{
	short int* wyjscie = (short int*) output_buf;
	
	for (unsigned int j = 0; j < n; j++)
		inputs[j].consumed = 0;
	
	for (unsigned long i = 0; i < 88.2 * tx_interval_ms; i++) {
		wyjscie[i] = 0;
		
		for (unsigned int j = 0; j < n; j++) {
			short int* dane = (short int*) inputs[j].data;
			
			if (2*i + 2 <= inputs[j].len) {
				if (SHRT_MAX - wyjscie[i] < dane[i])
					wyjscie[i] = SHRT_MAX;
				else if (SHRT_MIN - wyjscie[i] > dane[i])
					wyjscie[i] = SHRT_MIN;
				else
					wyjscie[i] += dane[i];
				
				inputs[j].consumed += 2;
			}
		}
	}
	
	*output_size = 176.4 * tx_interval_ms;
}

#endif
