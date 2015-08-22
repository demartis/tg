#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndfile.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <stdint.h>
#include <portaudio.h>

#define PA_SAMPLE_RATE 44100

int paudio_callback(const void *input_buffer,
			void *UNUSED(output_buffer),
			unsigned long frame_count,
			const PaStreamCallbackTimeInfo* UNUSED(time_info),
			PaStreamCallbackFlags UNUSED(status_flags),
			void *UNUSED(data))
{
	unsigned long i;
	for(i=0; i < frame_count; i++) {
		pa_buffers[0][write_pointer] = ((float *)input_buffer)[2*i];
		pa_buffers[1][write_pointer] = ((float *)input_buffer)[2*i + 1];
		if(++write_pointer == PA_BUFF_SIZE) write_pointer = 0;
	}
    
        return 0;
}

PaStream *create_portaudio_stream()
{
	PaStream *stream;

	PaError err = Pa_Initialize();
	if(err!=paNoError)
        	goto error;

	err = Pa_OpenDefaultStream(&stream,2,0,paFloat32,PA_SAMPLE_RATE,paFramesPerBufferUnspecified,paudio_callback,NULL);
	if(err!=paNoError)
		goto error;

	return stream;

error:
	fprintf(stderr,"Error opening audio input: %s\n", Pa_GetErrorText(err));
	pa_exit(err);
	return NULL;
}

int get_period(float *data, int size, double sample_rate, double *period, double *sigma)
{
	double estimate = 0;
	float largest_peak = 0;
	int i;
	for(i = sample_rate / 6; i < sample_rate / 2; i++) {
		if(i > size) break;
		if(data[i] > largest_peak) {
			largest_peak = data[i];
			estimate = i;
		}
	}
	if(estimate == 0) return -1;
	double delta = sample_rate * 0.005;
	double new_estimate = estimate;
	double sum = 0;
	double sq_sum = 0;
	int count = 0;
	int cycle = 1;
	for(;;) {
		int inf = floor(new_estimate * cycle - delta);
		int sup = ceil(new_estimate * cycle + delta);
		if(sup > size * 2 / 3)
			break;
		float max = 0;
		for(i = inf; i <= sup; i++)
			if(data[i] > max) {
				max = data[i];
				new_estimate = i;
			}
		if(max == 0) return -1;
		new_estimate /= cycle;
		fprintf(stderr,"cycle = %d new_estimate = %f\n",cycle,new_estimate/44100);
		if(new_estimate < estimate - delta || new_estimate > estimate + delta) return -1;
		if(inf > size / 3) {
			sum += new_estimate;
			sq_sum += new_estimate * new_estimate;
			count++;
		}
		cycle++;
	}
	estimate = sum / count;
	*period = estimate / sample_rate;
	*sigma = sqrt((sq_sum - count * estimate * estimate)/ (count-1)) / sample_rate;
	return 0;
}

int main(int argc, char **argv)
{
	if(argc != 2) {
		fprintf(stderr,"USAGE: %s filename\n",argv[0]);
		return 1;
	}

	SF_INFO sfinfo;
	SNDFILE *sfile;

	sfinfo.format = 0;

	sfile = sf_open(argv[1],SFM_READ,&sfinfo);
	if(!sfile) {
		fprintf(stderr,"Error in sf_open(): %s\n",sf_strerror(NULL));
		return 1;
	}

	if(sfinfo.channels != 1) {
		fprintf(stderr,"Channel count = %d != 1\n",sfinfo.channels);
		return 1;
	}

//	sfinfo.frames = 20 * sfinfo.samplerate;

	int sample_rate = sfinfo.samplerate;
	int sample_count = sfinfo.frames;
	float *samples;

	samples = malloc(2 * sample_count * sizeof(float));
	memset(samples,0,2 * sample_count * sizeof(float));

	fprintf(stderr,"sample_rate = %d\n",sample_rate);

	if(sfinfo.frames != sf_read_float(sfile,samples,sfinfo.frames)) {
		fprintf(stderr,"Error in sf_read_double()\n");
		return 1;
	}

	sf_close(sfile);

	int i;
/**/
	fftwf_plan plan_a,plan_b;
	int first_fft_size = sample_count/2 + 1;
	fftwf_complex *first_fft = malloc(first_fft_size * sizeof(fftwf_complex));

	plan_a = fftwf_plan_dft_r2c_1d(sample_count,samples,first_fft,FFTW_ESTIMATE);
	plan_b = fftwf_plan_dft_c2r_1d(sample_count,first_fft,samples,FFTW_ESTIMATE);

	fftwf_execute(plan_a);
	for(i=0;i<first_fft_size;i++) {
		if((uint64_t)sample_rate * i < (uint64_t)3000 * sample_count)
			first_fft[i] = 0;
	}
	fftwf_execute(plan_b);
/**/
	for(i=0;i<sample_count;i++)
		samples[i] = samples[i] * samples[i];

	float min = 1e20;
	for(i=0; i+sample_rate <= sample_count; i+=sample_rate) {
		int j;
		float max = 0;
		for(j=0; j<sample_rate; j++)
			if(samples[i+j] > max) max = samples[i+j];
		if(max < min) min = max;
	}

	int max_count = 0;
	int ms = sample_rate/1000;
	for(i=0;i<10*ms;i++) {
		if(samples[i] >= min) {
			if(max_count < 110*ms)
				max_count += 100;
		} else if(max_count > 0)
			max_count--;
	}
	for(i=0;i<sample_count;i++) {
		if(samples[i] > min) samples[i] = min;
		if(max_count >= 100*ms) samples[i] = 0;
		if(samples[i+10*ms] >= min) {
			if(max_count < 110*ms)
				max_count += 100;
		} else if(max_count > 0)
			max_count--;
	}

	double average = 0;
	for(i=0;i<sample_count;i++)
		average += samples[i];
	average /= sample_count;
	for(i=0;i<sample_count;i++)
		samples[i] -= average;

/*
	double *filtered_samples = malloc(sample_count * sizeof(double));
	for(i=0;i<sample_count;i++)
		filtered_samples[i] = samples[i];
*/

	fftwf_plan plan_c,plan_d;
	int second_fft_size = sample_count + 1;
	fftwf_complex *second_fft = malloc(second_fft_size * sizeof(fftwf_complex));

	plan_c = fftwf_plan_dft_r2c_1d(2*sample_count,samples,second_fft,FFTW_ESTIMATE);
	plan_d = fftwf_plan_dft_c2r_1d(2*sample_count,second_fft,samples,FFTW_ESTIMATE);

	fftwf_execute(plan_c);
	for(i=0;i<second_fft_size;i++) {
		if((uint64_t)sample_rate * i < (uint64_t)3000 * sample_count)
			second_fft[i] = second_fft[i] * conj(second_fft[i]);
		else
			second_fft[i] = 0;
	}
	fftwf_execute(plan_d);
	
	double period = 0;
	double sigma = 0;
	int err = get_period(samples,sample_count,sample_rate,&period,&sigma);
	if(err) printf("---\n");
	else printf("%f +- %f\n",period,sigma);

//	FILE *test = fopen("test","w");
//
//	for(i = 0; i < sample_count; i++) {
//		double t = (double) i / sample_rate;
//		fprintf(test,"%f %.12f\n",1000*t,samples[i]);
//	}
//
//	fclose(test);

	SF_INFO out_info;
	SNDFILE *out_sfile;

	out_info.samplerate = sample_rate;
	out_info.channels = 1;
	out_info.format = SF_FORMAT_WAV | SF_FORMAT_DOUBLE;

	out_sfile = sf_open("out.wav",SFM_WRITE,&out_info);
	if(!out_sfile) {
		fprintf(stderr,"Can not open out.wav\n");
		return 1;
	}

	float max_sample = 0;
	for(i = 100; i < sample_count; i++) {
		float x = samples[i] >= 0 ? samples[i] : -samples[i];
		max_sample = max_sample > x ? max_sample : x;
	}
	for(i = 0; i < sample_count; i++)
		samples[i] /= max_sample;

	fprintf(stderr,"Written %d samples to out.wav\n",sf_write_float(out_sfile, samples, sample_count));

	sf_close(out_sfile);

	return 0;
}
