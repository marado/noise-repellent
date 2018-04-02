/*
noise-repellent -- Noise Reduction LV2

Copyright 2016 Luciano Dato <lucianodato@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/
*/

/**
* \file stft_processor.c
* \author Luciano Dato
* \brief Contains an STFT processor abstraction suitable for real time processing
*/

#include <fftw3.h>
#include "fft_processor.c"

//STFT default values (Hardcoded for now)
#define FFT_SIZE 2048    //Size of the fft transform
#define BLOCK_SIZE 2048  //Size of the block of samples
#define INPUT_WINDOW_TYPE 3   //0 HANN 1 HAMMING 2 BLACKMAN 3 VORBIS Input windows for STFT algorithm
#define OUTPUT_WINDOW_TYPE 3  //0 HANN 1 HAMMING 2 BLACKMAN 3 VORBIS Output windows for STFT algorithm
#define OVERLAP_FACTOR 4 //4 is 75% overlap Values bigger than 4 will rescale correctly (if Vorbis windows is not used)

/**
* STFT processor struct.
*/
typedef struct
{
  int fft_size;
  fftwf_plan forward;
  fftwf_plan backward;
  int block_size;
  int window_option_input;    //Type of input Window for the STFT
  int window_option_output;   //Type of output Window for the STFT
  int overlap_factor;         //oversampling factor for overlap calculations
  float overlap_scale_factor; //Scaling factor for conserving the final amplitude
  int hop;                    //Hop size for the STFT
  int input_latency;
  int read_position;
  float *input_window;
  float *output_window;
  float *in_fifo;
  float *out_fifo;
  float *output_accum;
  float *input_fft_buffer;
  float *output_fft_buffer;

  //FFT processor instance
	FFTprocessor *fft_processor;
} STFTprocessor;

/**
* Wrapper for getting the pre and post processing windows and adequate scaling factor.
*/
void stft_p_pre_and_post_window(STFTprocessor *self)
{
  float sum = 0.f;
  
  //Input window
  switch (self->window_option_input)
  {
  case 0:                                                // HANN
    fft_window(self->input_window, self->block_size, 0); //STFT input window
    break;
  case 1:                                                //HAMMING
    fft_window(self->input_window, self->block_size, 1); //STFT input window
    break;
  case 2:                                                //BLACKMAN
    fft_window(self->input_window, self->block_size, 2); //STFT input window
    break;
  case 3:                                                //VORBIS
    fft_window(self->input_window, self->block_size, 3); //STFT input window
    break;
  }

  //Output window
  switch (self->window_option_output)
  {
  case 0:                                                 // HANN
    fft_window(self->output_window, self->block_size, 0); //STFT input window
    break;
  case 1:                                                 //HAMMING
    fft_window(self->output_window, self->block_size, 1); //STFT input window
    break;
  case 2:                                                 //BLACKMAN
    fft_window(self->output_window, self->block_size, 2); //STFT input window
    break;
  case 3:                                                 //VORBIS
    fft_window(self->output_window, self->block_size, 3); //STFT input window
    break;
  }

  //Once windows are initialized we can obtain 
  //the scaling necessary for perfect reconstruction using Overlapp Add
  for (int i = 0; i < self->block_size; i++)
    sum += self->input_window[i] * self->output_window[i];

  self->overlap_scale_factor = (sum / (float)(self->block_size));
}

/**
* Adds zeros to the spectrum in order to complete the spectrum if block_size != fft_size.
*/
void stft_p_zeropad(STFTprocessor *self)
{
  int k;
  int number_of_zeros = self->fft_size - self->block_size;

  //This adds zeros at the end. The right way should be an equal amount at the sides
  for (k = 0; k <= number_of_zeros - 1; k++)
  {
    self->input_fft_buffer[self->block_size - 1 + k] = 0.f;
  }
}

/**
* Does the analysis part of the stft for current block.
*/
void stft_p_analysis(STFTprocessor *self)
{
  int k;

  if (self->block_size < self->fft_size)
  {
    stft_p_zeropad(self);
  }

  //Windowing the frame input values in the center (zero-phasing)
  for (k = 0; k < self->fft_size; k++)
  {
    self->input_fft_buffer[k] *= self->input_window[k];
  }

  //Do transform
  fftwf_execute(self->forward);
}

/**
* Does the synthesis part of the stft for current block and then does the OLA method to
* enable the final output.
*/
void stft_p_synthesis(STFTprocessor *self)
{
  int k;

  //Do inverse transform
  fftwf_execute(self->backward);

  //Normalizing value
  for (k = 0; k < self->fft_size; k++)
  {
    self->input_fft_buffer[k] = self->input_fft_buffer[k] / self->fft_size;
  }

  //Windowing and scaling
  for (k = 0; k < self->fft_size; k++)
  {
    self->input_fft_buffer[k] = (self->output_window[k] * self->input_fft_buffer[k]) / (self->overlap_scale_factor * self->overlap_factor);
  }

  //OVERLAPP-ADD
  //Accumulation
  for (k = 0; k < self->block_size; k++)
  {
    self->output_accum[k] += self->input_fft_buffer[k];
  }

  //Output samples up to the hop size
  for (k = 0; k < self->hop; k++)
  {
    self->out_fifo[k] = self->output_accum[k];
  }

  //shift FFT accumulator the hop size
  memmove(self->output_accum, self->output_accum + self->hop,
          self->block_size * sizeof(float));

  //move input FIFO
  for (k = 0; k < self->input_latency; k++)
  {
    self->in_fifo[k] = self->in_fifo[k + self->hop];
  }
}

/**
* Returns the latency needed to be reported to the host.
*/
int stft_p_get_latency(STFTprocessor *self)
{
  return self->input_latency;
}

/**
* Calls the fft processor to apply the processing to current block.
*/
void stft_p_processing(STFTprocessor *self, float *enable)
{
  //Call processing  with the obtained fft transform
  //when stft analysis is applied it resides in output_fft_buffer
  fft_p_run(self->fft_processor, self->output_fft_buffer, enable);
}

/**
* Initializes all dynamics arrays with zeros.
*/
void stft_p_reset(STFTprocessor *self)
{
  //Reset all arrays
  initialize_array(self->input_fft_buffer, 0.f, self->fft_size);
  initialize_array(self->output_fft_buffer, 0.f, self->fft_size);
  initialize_array(self->input_window, 0.f, self->fft_size);
  initialize_array(self->output_window, 0.f, self->fft_size);
  initialize_array(self->in_fifo, 0.f, self->block_size);
  initialize_array(self->out_fifo, 0.f, self->block_size);
  initialize_array(self->output_accum, 0.f, self->block_size * 2);
}

/**
* STFT processor initialization and configuration.
*/
STFTprocessor *
stft_p_init(int sample_rate)
{
  //Allocate object
  STFTprocessor *self = (STFTprocessor *)malloc(sizeof(STFTprocessor));

  //self configuration
  self->block_size = BLOCK_SIZE;
  self->fft_size = FFT_SIZE;
  self->window_option_input = INPUT_WINDOW_TYPE;
  self->window_option_output = OUTPUT_WINDOW_TYPE;
  self->overlap_factor = OVERLAP_FACTOR;
  self->hop = self->fft_size / self->overlap_factor;
  self->input_latency = self->fft_size - self->hop;
  self->read_position = self->input_latency;

  //Individual array allocation

  //STFT window related
  self->input_window = (float *)malloc(self->fft_size * sizeof(float));
  self->output_window = (float *)malloc(self->fft_size * sizeof(float));

  //fifo buffer init
  self->in_fifo = (float *)malloc(self->block_size * sizeof(float));
  self->out_fifo = (float *)malloc(self->block_size * sizeof(float));

  //buffer for OLA
  self->output_accum = (float *)malloc((self->block_size * 2) * sizeof(float));

  //FFTW related
  self->input_fft_buffer = (float *)fftwf_malloc(self->fft_size * sizeof(float));
  self->output_fft_buffer = (float *)fftwf_malloc(self->fft_size * sizeof(float));
  self->forward = fftwf_plan_r2r_1d(self->fft_size, self->input_fft_buffer,
                                    self->output_fft_buffer, FFTW_R2HC,
                                    FFTW_ESTIMATE);
  self->backward = fftwf_plan_r2r_1d(self->fft_size, self->output_fft_buffer,
                                     self->input_fft_buffer, FFTW_HC2R,
                                     FFTW_ESTIMATE);

  //Initialize all arrays with zeros
  stft_p_reset(self);

  //Window combination initialization (pre processing window post processing window)
  stft_p_pre_and_post_window(self);

  //Spectral processor related
	self->fft_processor = fft_p_init(self->fft_size, sample_rate, self->hop);

  return self;
}

/**
* Free allocated memory.
*/
void stft_p_free(STFTprocessor *self)
{
  fftwf_free(self->input_fft_buffer);
  fftwf_free(self->output_fft_buffer);
  fftwf_destroy_plan(self->forward);
  fftwf_destroy_plan(self->backward);
  free(self->input_window);
  free(self->output_window);
  free(self->in_fifo);
  free(self->out_fifo);
  free(self->output_accum);
  fft_p_free(self->fft_processor);
  free(self);
}

/**
* Runs the STFT processing for the given signal by the host.
*/
void stft_p_run(STFTprocessor *self, int n_samples, const float *input, float *output,
              float *enable)
{
  int k;

  for (k = 0; k < n_samples; k++)
  {
    //Read samples given by the host and write samples to the host
    self->in_fifo[self->read_position] = input[k];
    output[k] = self->out_fifo[self->read_position - self->input_latency];
    self->read_position++;

    if (self->read_position >= self->block_size)
    {
      //Fill the buffer and reset the read position
      memcpy(self->input_fft_buffer, self->in_fifo, sizeof(float) * self->block_size);
      self->read_position = self->input_latency;

      //Do analysis
      stft_p_analysis(self);

      //Do processing
      stft_p_processing(self, enable);

      //Do synthesis
      stft_p_synthesis(self);
    }
  }
}