/*-
 * Copyright (c) 2016 Semihalf.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <sched.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>

#include "bench.h"

#define min(a,b) ((a) < (b)) ? (a) : (b)
#define max(a,b) ((a) < (b)) ? (b) : (a)

#define CLOCK_TYPE CLOCK_MONOTONIC_RAW

double student_1_30[] = {0, 12.71,
4.303,3.182,2.776,2.571,2.447,2.365,2.306,2.262,2.228,2.201,2.179,
2.160,2.145,2.131,2.120,2.110,2.101,2.093,2.086,2.080,2.074,2.069,
2.064,2.060,2.056,2.052,2.048,2.045,2.042};

double student_sparse[][2] = {
{30, 2.042},
{40, 2.021},
{60, 2.000},
{80, 1.990},
{100, 1.984},
{1000, 1.962},
};

double t_val(unsigned n)
{
	if (n <= 30)
		return student_1_30[n];

	size_t i;
	size_t len = sizeof(student_sparse)/sizeof(*student_sparse);
	for (i = 1; i < len; i++) {
		if (student_sparse[i][0] > n)
			return student_sparse[i-1][1];
	}
	return student_sparse[i-1][1];
}

uint64_t getclock()
{
	struct timespec ts;
	if (clock_gettime(CLOCK_TYPE, &ts) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#define MAXTHREADS 16

struct bench_shared {
	uint32_t barrier;
	struct thrarg thrargs[MAXTHREADS];
};

struct bench_shared *shared;

static void *alloc_shared(size_t size)
{
	void *ptr = mmap(NULL, size,
		PROT_READ|PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (ptr == NULL) {
		perror("mmap");
		exit(1);
	}
	return ptr;
}

__attribute__((unused))
static void free_shared(void *ptr, size_t size)
{
	munmap(ptr, size);
}

static void barrier_wait(uint32_t *barrier)
{
	uint32_t val = __atomic_sub_fetch(barrier, 1, __ATOMIC_RELAXED);

	while (val != 0)
		val = __atomic_load_n(barrier, __ATOMIC_RELAXED);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void *thread(void *arg)
{
	uint64_t t1, t2;
	size_t tid = (size_t)arg;
	struct thrarg *thrarg = &shared->thrargs[tid];

	thrarg->params.init(thrarg);

	barrier_wait(&shared->barrier);

	t1 = getclock();

	thrarg->params.benchmark(thrarg);

	t2=getclock();

	thrarg->result.avg = (t2 - t1)/(double)(thrarg->params.iters);
	thrarg->result.sum = (t2 - t1);
	return NULL;
}

void benchmark_once_thread(struct thrarg *thrarg, unsigned iters)
{
	const int new_thread = 0;
	size_t i;
	unsigned nthreads = thrarg->params.threads;
	struct thrarg *thrargs;
	pthread_t threads[nthreads];
	pthread_attr_t attr;
	cpu_set_t c;
	const bool affinity = true;

	thrarg->params.iters = iters;

	if (!shared)
		shared = alloc_shared(sizeof(struct bench_shared));

	thrargs = shared->thrargs;
	shared->barrier = nthreads;

	pthread_attr_init(&attr);

	for (i=0; i < nthreads; i++) {
		thrargs[i] = *thrarg;
		thrargs[i].params.id = i;
	}

	i = (new_thread) ? 0 : 1;
	for (; i < nthreads; i++) {
		if (affinity) {
			CPU_ZERO(&c);
			CPU_SET(i, &c);
			pthread_attr_setaffinity_np(&attr, sizeof(c), &c);
		}
		pthread_create(&threads[i], &attr, thread, (void *)i);
	}

	if (!new_thread)
		thread((void *)(size_t)0);

	i = (new_thread) ? 0 : 1;
	for (; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	thrarg->result.avg = thrargs[0].result.avg;
	thrarg->result.sum = thrargs[0].result.sum;
}

void benchmark_once_fork(struct thrarg *thrarg, unsigned iters)
{
	size_t i;
	unsigned nthreads = thrarg->params.threads;
	struct thrarg *thrargs;
	cpu_set_t c;
	pid_t pids[nthreads];

	if (!shared)
		shared = alloc_shared(sizeof(struct bench_shared));

	thrargs = shared->thrargs;
	shared->barrier = nthreads;

	thrarg->params.iters = iters;

	for (i=0; i < nthreads; i++) {
		thrargs[i] = *thrarg;
		thrargs[i].params.id = i;
	}

	for (i=0; i < nthreads; i++) {
		CPU_ZERO(&c);
		CPU_SET(i, &c);
		pids[i] = fork();
		if (!pids[i]) {
			pthread_setaffinity_np(pthread_self(), sizeof(c), &c);
			thread((void *)i);
			exit(0);
		}
	}

	for (i=0; i < nthreads; i++)
		waitpid(pids[i], NULL, 0);

	thrarg->result.avg = thrargs[0].result.avg;
	thrarg->result.sum = thrargs[0].result.sum;
}

void (*bench_once)(struct thrarg *, unsigned) = benchmark_once_thread;

static inline double sqr(double x)
{
	return x*x;
}

double avg(size_t n, double samples[n])
{
	size_t i;
	double sum = 0.0;
	for (i = 0; i < n; i++)
		sum += samples[i];
	double avg = sum/n;
	return avg;
}

double stdev(size_t n, double samples[n], double avg)
{
	size_t i;
	double var = 0.0;

	for (i = 0; i < n; i++)
		var += sqr(avg - samples[i]);
	if (n <= 1)
		n = 2;
	double stdev = sqrt(var/(n-1));
	return stdev;
}

static bool bench_try(struct thrarg *thrarg, unsigned iters)
{
	const unsigned min_samples = 10;
	double sum, avg, std_dev = 1.0, u = 1.0, e = 1.0;
	size_t n, i;
	bool print_samples = thrarg->params.print_samples;
	bool success = false;
	const unsigned max_samples = thrarg->params.max_samples ?
					thrarg->params.max_samples : 400;
	const double error = thrarg->params.max_error ?
			thrarg->params.max_error / 100.0 : 0.05;
	double *samples = (double *)calloc(max_samples, sizeof(double));

	if (max_samples < min_samples)
		return false;

	sum = 0.0;
	avg = 0.0;
	n = 0;
	for (i = 0; i < max_samples; i++) {
		bench_once(thrarg, iters);
		samples[i] = thrarg->result.avg;
		sum += samples[i];
		n = i + 1;
		if (n < min_samples)
			continue;
		avg = sum/n;
		std_dev = stdev(n, samples, avg);
		double t = t_val(n);
		u = std_dev * t;
		e = u/avg;
		if (e < error) {
			success = true;
			break;
		}
	}

	thrarg->result.avg = avg;
	thrarg->result.samples = n;
	thrarg->result.iters = iters;
	thrarg->result.sum = sum;
	thrarg->result.sdev = std_dev;
	thrarg->result.u = u;
	thrarg->result.err = e;

	if (print_samples) {
		for (i = 0; i < n; i++)
			fprintf(stderr, "%f\n", samples[i]);
	}

	fprintf(stderr, "i = %d n = %zd sdev = %f u = %f e = %f a = %f\n",
		iters, n, std_dev, u, e, avg);
	return success;
}

int benchmark_auto(struct thrarg *thrarg)
{
	const unsigned max_iters = UINT_MAX;
	const unsigned min_iters = 10;
	unsigned iters;
	unsigned long mt = thrarg->params.min_time;
	const double min_time_ns = mt ? (double)mt : 1000*1000;
	bool success;
	size_t i;
	double last_error = 100.0;
	struct thrarg last_arg;
	double error;

	for (iters = min_iters; iters < max_iters; iters *= 2) {
		bench_once(thrarg, iters);
		if (thrarg->result.sum > min_time_ns)
			break;
	}

	if (iters > max_iters) {
		return -ENOSPC;
	}

	for (i = 0; i < 64 && iters <= max_iters; i++) {
		size_t j;
		last_arg = *thrarg;
		error = 0.0;
		success = true;
		for (j=0; j<3; j++) {
			success = min(bench_try(thrarg, iters), success);
			error = max(thrarg->result.err,error);
		}
		if (success)
			break;
		if (error > last_error) {
			//back off
			*thrarg = last_arg;
			break;
		}
		last_error = error;
		iters *= 2;
	}

	return success;
}

int benchmark_once(struct thrarg *thrarg)
{
	bench_once(thrarg, thrarg->params.iters);
	return 0;
}
