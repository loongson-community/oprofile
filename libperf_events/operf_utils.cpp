/**
 * @file operf_utils.cpp
 * Helper methods for perf_events-based OProfile.
 *
 * @remark Copyright 2011 OProfile authors
 * @remark Read the file COPYING
 *
 * Created on: Dec 7, 2011
 * @author Maynard Johnson
 * (C) Copyright IBM Corp. 2011
 *
 * Modified by Maynard Johnson <maynardj@us.ibm.com>
 * (C) Copyright IBM Corporation 2012
 *
 */

#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <libelf.h>
#include <gelf.h>
#include <cverb.h>
#include <iostream>
#include "operf_counter.h"
#include "operf_utils.h"
#ifdef HAVE_LIBPFM
#include <perfmon/pfmlib.h>
#endif
#include "op_types.h"
#include "operf_process_info.h"
#include "operf_kernel.h"
#include "operf_sfile.h"



extern verbose vmisc;
extern volatile bool quit;
extern operf_read operfRead;
extern int sample_reads;
extern unsigned int pagesize;
extern char * app_name;
extern verbose vperf;

using namespace std;

map<pid_t, operf_process_info *> process_map;
struct operf_mmap kernel_mmap;


static struct operf_transient trans;
static bool sfile_init_done;

/* The handling of mmap's for a process was a bit tricky to get right, in particular,
 * the handling of what I refer to as "deferred mmap's" -- i.e., when we receive an
 * mmap event for which we've not yet received a comm event (so we don't know app name
 * for the process).  I have left in some debugging code here (compiled out via #ifdef)
 * so we can easily test and validate any changes we ever may need to make to this code.
 */
//#define _TEST_DEFERRED_MAPPING
#ifdef _TEST_DEFERRED_MAPPING
static bool do_comm_event;
static event_t comm_event;
#endif


/* Some architectures (e.g., ppc64) do not use the same event value (code) for oprofile
 * and for perf_events.  The operf-record process requires event values that perf_events
 * understands, but the operf-read process requires oprofile event values.  The purpose of
 * the following method is to map the operf-record event value to a value that
 * opreport can understand.
 */
#if (defined(__powerpc__) || defined(__powerpc64__))
#define NIL_CODE ~0U
static bool _get_codes_for_match(unsigned int pfm_idx, const char name[],
                                 vector<operf_event_t> * evt_vec)
{
	unsigned int num_events = evt_vec->size();
	int tmp_code, ret;
	char evt_name[OP_MAX_EVT_NAME_LEN];
	char * grp_name;
	unsigned int events_converted = 0;
	for (unsigned int i = 0; i < num_events; i++) {
		operf_event_t event = (*evt_vec)[i];
		if (event.evt_code != NIL_CODE) {
			events_converted++;
			continue;
		}
		memset(evt_name, 0, OP_MAX_EVT_NAME_LEN);
		if (!strcmp(event.name, "CYCLES")) {
			strcpy(evt_name ,"PM_CYC") ;
		} else if ((grp_name = strstr(event.name, "_GRP"))) {
			strncpy(evt_name, event.name, grp_name - event.name);
		} else {
			strncpy(evt_name, event.name, strlen(event.name));
		}
		if (strncmp(name, evt_name, OP_MAX_EVT_NAME_LEN))
			continue;
		ret = pfm_get_event_code(pfm_idx, &tmp_code);
		if (ret != PFMLIB_SUCCESS) {
			string evt_name_str = event.name;
			string msg = "libpfm cannot find event code for " + evt_name_str +
					"; cannot continue";
			throw runtime_error(msg);
		}
		event.evt_code = tmp_code;
		(*evt_vec)[i] = event;
		events_converted++;
		cverb << vperf << "Successfully converted " << event.name << " to perf_event code "
		      << hex << tmp_code << endl;
	}
	return (events_converted == num_events);
}

bool OP_perf_utils::op_convert_event_vals(vector<operf_event_t> * evt_vec)
{
	unsigned int i, count;
	char name[256];
	int ret;
	for (unsigned int i = 0; i < evt_vec->size(); i++) {
		operf_event_t event = (*evt_vec)[i];
		event.evt_code = NIL_CODE;
		(*evt_vec)[i] = event;
	}
	pfm_initialize();
	ret = pfm_get_num_events(&count);
	if (ret != PFMLIB_SUCCESS)
		throw runtime_error("Unable to use libpfm to obtain event code; cannot continue");
	for(i =0 ; i < count; i++)
	{
		ret = pfm_get_event_name(i, name, 256);
		if (ret != PFMLIB_SUCCESS)
			continue;
		if (_get_codes_for_match(i, name, evt_vec))
			break;
	}
	return (i != count);
}

#endif


static inline void update_trans_last(struct operf_transient * trans)
{
	trans->last = trans->current;
	trans->last_anon = trans->anon;
	trans->last_pc = trans->pc;
}


static void __handle_comm_event(event_t * event)
{
#ifdef _TEST_DEFERRED_MAPPING
	if (!do_comm_event) {
		comm_event = event;
		return;
	}
#endif
	cverb << vperf << "PERF_RECORD_COMM for " << event->comm.comm << endl;
	map<pid_t, operf_process_info *>::iterator it;
	it = process_map.find(event->comm.pid);
	if (it == process_map.end()) {
		operf_process_info * proc = new operf_process_info(event->comm.pid,
		                                                   app_name ? app_name : event->comm.comm,
		                                                   app_name == NULL, true);
		cverb << vperf << "Adding new proc info to collection for PID " << event->comm.pid << endl;
		process_map[event->comm.pid] = proc;
	} else {
		// sanity check -- should not get a second COMM event for same PID
		if (it->second->is_valid()) {
			cverb << vperf << "Received extraneous COMM event for " << event->comm.comm
			      << ", PID " << event->comm.pid << endl;
		} else {
			cverb << vperf << "Processing deferred mappings" << endl;
			it->second->process_deferred_mappings(event->comm.comm);
		}
	}
}

static void __handle_mmap_event(event_t * event)
{
	struct operf_mmap mapping;
	mapping.start_addr = event->mmap.start;
        strcpy(mapping.filename, event->mmap.filename);

	mapping.end_addr = (event->mmap.len == 0ULL)? 0ULL : mapping.start_addr + event->mmap.len - 1;
	mapping.pgoff = event->mmap.pgoff;
	mapping.pid = event->mmap.pid;

	cverb << vperf << "PERF_RECORD_MMAP for " << event->mmap.filename << endl;
	cverb << vperf << "\tstart_addr: " << hex << mapping.start_addr;
	cverb << vperf << "; pg offset: " << mapping.pgoff << endl;

	if (event->header.misc & PERF_RECORD_MISC_KERNEL) {
		mapping.buildid_valid = OP_perf_utils::get_build_id(mapping.buildid);
		if (!mapping.buildid_valid) {
			mapping.checksum = OP_perf_utils::get_checksum_for_file(mapping.filename);
			cverb << vmisc << "checksum for file " << mapping.filename << ": "
			      << hex << mapping.checksum << endl;
		} else {
			cverb << vmisc << "buildid for file " << mapping.filename << ": "
			      << mapping.buildid << endl;
		}
		//mapping.checksum = 0xdadd0000abcd1234;
		kernel_mmap = mapping;
	} else {
		map<pid_t, operf_process_info *>::iterator it;
		it = process_map.find(event->mmap.pid);
		if (it == process_map.end()) {
			// Create a new proc info object, but mark it invalid since we have
			// not yet received a COMM event for this PID.
			operf_process_info * proc = new operf_process_info(event->mmap.pid, app_name ? app_name : "",
			                                                                             app_name == NULL, false);
			proc->add_deferred_mapping(mapping);
			cverb << vperf << "Added deferred mapping " << event->mmap.filename
					<< " for new process_info object" << endl;
			process_map[event->mmap.pid] = proc;
#ifdef _TEST_DEFERRED_MAPPING
			if (!do_comm_event) {
				do_comm_event = true;
				__handle_comm_event(comm_event, out);
			}
#endif
		} else if (!it->second->is_valid()) {
			it->second->add_deferred_mapping(mapping);
			cverb << vperf << "Added deferred mapping " << event->mmap.filename
					<< " for existing but incomplete process_info object" << endl;
		} else {
			it->second->process_new_mapping(mapping);
		}
	}
}
static void __handle_sample_event(event_t * event)
{
	struct sample_data data;
	bool early_match = false;
	operf_process_info * proc;
	const u64 type = OP_BASIC_SAMPLE_FORMAT;

	u64 *array = event->sample.array;

	if (type & PERF_SAMPLE_IP) {
		data.ip = event->ip.ip;
		array++;
	}

	if (type & PERF_SAMPLE_TID) {
		u_int32_t *p = (u_int32_t *)array;
		data.pid = p[0];
		data.tid = p[1];
		array++;
	}

	data.id = ~0ULL;
	if (type & PERF_SAMPLE_ID) {
		data.id = *array;
		array++;
	}

	if (type & PERF_SAMPLE_STREAM_ID) {
		data.stream_id = *array;
		array++;
	}

	if (type & PERF_SAMPLE_CPU) {
		u_int32_t *p = (u_int32_t *)array;
		data.cpu = *p;
		array++;
	}
	if (event->header.misc & PERF_RECORD_MISC_KERNEL) {
		trans.in_kernel = 1;
	} else if (event->header.misc & PERF_RECORD_MISC_USER) {
		trans.in_kernel = 0;
	} else {
		// TODO: For now, we'll discard hypervisor and guest kernel/
		// guest user samples.  We should at least log what we're
		// throwing away.
		cverb << vmisc << "Discarding sample that is neither user nor kernel domain" << endl;
		goto out;
	}


	/* Early versions of the Linux Performance Events Subsystem were a bit
	 * buggy and would record samples for processes other than the requested
	 * process ID.  The code below basically ignores these superfluous samples
	 * except for the case of PID 0.  If this case occurs right away when
	 * the static variable trans.tgid is still holding its initial value of 0,
	 * then we would incorrectly find trans.tgid and data.pid matching, and
	 * and make wrong assumptions from that match -- ending seg fault.  So we
	 * will bail out early if we see a sample for PID 0 coming in.
	 */
	if (data.pid == 0) {
		cverb << vmisc << "Discarding sample for PID 0" << endl;
		goto out;
	}

	// TODO: handle callchain
	cverb << vperf << "(IP, " <<  event->header.misc << "): " << dec << data.pid << "/"
	      << data.tid << ": " << hex << (unsigned long long)data.ip
	      << endl << "\tdata ID: " << data.id << "; data stream ID: " << data.stream_id << endl;

	// Verify the sample.
	trans.event = operfRead.get_eventnum_by_perf_event_id(data.id);
	if (trans.event < 0) {
		cerr << "Event num " << trans.event << " for id " << data.id
		     << " is invalid. Skipping sample." << endl;
		goto out;
	}
	// Do the common case first.  For the "no_vmlinux" case, start_addr and end_addr will be
	// zero, so need to make sure we detect that and fall through.
	if (trans.in_kernel) {
		if (trans.image_name && trans.tgid == data.pid) {
			// For the no_vmlinux case . . .
			if ((trans.start_addr == 0ULL) && (trans.end_addr == 0ULL)) {
				trans.pc = data.ip;
				early_match = true;
			// For the case where a vmlinux file is passed in . . .
			} else if (data.ip >= trans.start_addr && data.ip <= trans.end_addr) {
				trans.pc = data.ip;
				early_match = true;
			}
		}
	} else {
		if (trans.tgid == data.pid && data.ip >= trans.start_addr && data.ip <= trans.end_addr) {
			trans.tid = data.tid;
			trans.pc = data.ip - trans.start_addr;
			early_match = true;
		}
	}
	if (early_match) {
		operf_sfile_find(&trans);
		/*
		 * trans.current may be NULL if a kernel sample falls through
		 * the cracks, or if it's a sample from an anon region we couldn't find
		 */
		if (trans.current)
			/* log the sample or arc */
			operf_sfile_log_sample(&trans);

		update_trans_last(&trans);
		goto out;
	}

	if (trans.tgid == data.pid) {
		proc = trans.cur_procinfo;
	} else {
		// Find operf_process info for data.tgid.
		map<pid_t, operf_process_info *>::const_iterator it = process_map.find(data.pid);
		if (it != process_map.end() && (it->second->appname_valid())) {
			proc = it->second;
			trans.cur_procinfo = proc;
		} else {
			// TODO
			/* This can happen for the following reasons:
			 *   - We get a sample before getting a COMM or MMAP
			 *     event for the process being profiled
			 *   - The COMM event has been processed, but since that
			 *     only gives 16 chars of the app name, we don't
			 *     have a valid app name yet
			 *   - The kernel incorrectly records a sample for a
			 *     process other than the one we requested (not
			 *     likely -- this would be a kernel bug if it did)
			 *
			 * Need to look into caching these discarded samples and trying to
			 * process them after we have a valid app name recorded.  This could
			 * cause a lot of thrashing about.  But at the very least,
			 * we need to log the lost sample.
			*/
			cverb << vmisc << "Discarding sample for a process other than the one requested" << endl;
			goto out;
		}
	}

	// Now find mmapping that contains the data.ip address.
	// Use that mmapping to set fields in trans.
	const struct operf_mmap * map;
	if (trans.in_kernel) {
		map = &kernel_mmap;
	} else {
		map = proc->find_mapping_for_sample(data.ip);
	}
	if (map) {
		cverb << vperf << "Found mmap for sample" << endl;
		if (map->buildid_valid) {
			trans.buildid = map->buildid;
			trans.buildid_valid = true;
		} else {
			trans.checksum = map->checksum;
			trans.buildid_valid = false;
			// checksums are guaranteed unique, so we need the filename to be certain
		}
		trans.image_name = map->filename;
		trans.app_filename = proc->get_app_name().c_str();
		trans.start_addr = map->start_addr;
		trans.end_addr = map->end_addr;
		trans.tgid = data.pid;
		trans.tid = data.tid;
		if (trans.in_kernel)
			trans.pc = data.ip;
		else
			trans.pc = data.ip - trans.start_addr;

		trans.sample_id = data.id;
		trans.current = operf_sfile_find(&trans);
		/*
		 * trans.current may be NULL if a kernel sample falls through
		 * the cracks, or if it's a sample from an anon region we couldn't find
		 */
		if (trans.current)
			/* log the sample or arc */
			operf_sfile_log_sample(&trans);

		update_trans_last(&trans);
	} else {
		// TODO: log lost sample due to no mapping
		cverb << vmisc << "Discarding sample where no mapping was found" << endl;
		return;
	}
out:
	return;
}


/* This function is used by operf_read::convertPerfData() to convert perf-formatted
 * data to oprofile sample data files.  After the header information in the perf data file,
 * the next piece of data is the PERF_RECORD_COMM record which tells us the name of the
 * application/command being profiled.  This is followed by PERF_RECORD_MMAP records
 * which indicate what binary executables and libraries were mmap'ed into process memory
 * when profiling began.  Additional PERF_RECORD_MMAP records may appear later in the data
 * stream (e.g., dlopen for single-process profiling or new process startup for system-wide
 * profiling.  For such additional PERF_RECORD_MMAP records, it's possible that samples
 * may be collected and written to the perf data file for those mmap'ed binaries prior
 * to the reception/recording of the corresponding PERF_RECORD_MMAP record, in which case,
 * those samples are discarded and we increment the appropriate "lost sample" stat.
 * TODO:   What's the name of the stat we increment?
 */
int OP_perf_utils::op_write_event(event_t * event)
{
#if 0
	if (event->header.type < PERF_RECORD_MAX) {
		cverb << vperf << "PERF_RECORD type " << hex << event->header.type << endl;
	}
#endif

	switch (event->header.type) {
	case PERF_RECORD_SAMPLE:
		__handle_sample_event(event);
		return 0;
	case PERF_RECORD_MMAP:
		__handle_mmap_event(event);
		return 0;
	case PERF_RECORD_COMM:
		if (!sfile_init_done) {
			operf_sfile_init();
			sfile_init_done = true;
		}
		__handle_comm_event(event);
		return 0;
	case PERF_RECORD_EXIT:
		cverb << vperf << "PERF_RECORD_EXIT found in trace" << endl;
		return 0;
	default:
		// OK, ignore all other header types.
		cverb << vperf << "No matching event type for " << hex << event->header.type << endl;
		return 0;
	}
}


void OP_perf_utils::op_perfrecord_sigusr1_handler(int sig __attribute__((unused)),
		siginfo_t * siginfo __attribute__((unused)),
		void *u_context __attribute__((unused)))
{
	quit = true;
}

int OP_perf_utils::op_read_from_stream(ifstream & is, char * buf, streamsize sz)
{
	int rc = 0;
	is.read(buf, sz);
	if (!is.eof() && is.fail()) {
		cerr << "Internal error:  Failed to read from input file." << endl;
		rc = -1;
	} else {
		rc = is.gcount();
	}
	return rc;
}

int OP_perf_utils::op_write_output(int output, void *buf, size_t size)
{
	int sum = 0;
	while (size) {
		int ret = write(output, buf, size);

		if (ret < 0) {
			string errmsg = "Internal error:  Failed to write to output file. errno is ";
			errmsg += strerror(errno);
			throw runtime_error(errmsg);
		}

		size -= ret;
		buf = (char *)buf + ret;
		sum  += ret;
	}
	return sum;
}


static void op_record_process_exec_mmaps(pid_t pid, pid_t tgid, int output_fd, operf_record * pr)
{
	char fname[PATH_MAX];
	FILE *fp;

	snprintf(fname, sizeof(fname), "/proc/%d/maps", tgid);

	fp = fopen(fname, "r");
	if (fp == NULL) {
		// Process must have exited already or invalid pid.
		cverb << vperf << "couldn't open " << fname << endl;
		return;
	}

	while (1) {
		char line_buffer[BUFSIZ];
		char perms[5], pathname[PATH_MAX], dev[16];
		unsigned long long start_addr, end_addr, offset;
		u_int32_t inode;

		memset(pathname, '\0', sizeof(pathname));
		struct mmap_event mmap;
		size_t size;
		mmap.header.type = PERF_RECORD_MMAP;
		mmap.header.misc = PERF_RECORD_MISC_USER;

		if (fgets(line_buffer, sizeof(line_buffer), fp) == NULL)
			break;

		sscanf(line_buffer, "%llx-%llx %s %llx %s %d %s",
				&start_addr, &end_addr, perms, &offset, dev, &inode, pathname);
		if (perms[2] == 'x') {
			char *imagename = strchr(pathname, '/');

			if (imagename == NULL)
				imagename = strstr(pathname, "[vdso]");

			if (imagename == NULL)
				continue;

			size = strlen(imagename) + 1;
			strcpy(mmap.filename, imagename);
			size = align_64bit(size);
			mmap.start = start_addr;
			mmap.len = end_addr - mmap.start;
			mmap.pid = tgid;
			mmap.tid = pid;
			mmap.header.size = (sizeof(mmap) -
					(sizeof(mmap.filename) - size));
			int num = OP_perf_utils::op_write_output(output_fd, &mmap, mmap.header.size);
			cverb << vperf << "Created MMAP event for " << imagename << endl;
			pr->add_to_total(num);
		}
	}

	fclose(fp);
	return;
}

static int _record_one_process_info(pid_t pid, operf_record * pr,
                                    int output_fd)
{
	struct comm_event comm;
	char fname[PATH_MAX];
	char buff[BUFSIZ];
	FILE *fp;
	pid_t tgid = 0;
	size_t size = 0;
	DIR *tids;
	struct dirent dirent, *next;
	int ret = 0;

	snprintf(fname, sizeof(fname), "/proc/%d/status", pid);
	fp = fopen(fname, "r");
	if (fp == NULL) {
		// Process must have finished or invalid PID passed into us.
		// We'll bail out now.
		cerr << "Unable to find process information for PID " << pid << "." << endl;
		cverb << vperf << "couldn't open " << fname << endl;
		return -1;
	}

	memset(&comm, 0, sizeof(comm));
	while (!comm.comm[0] || !comm.pid) {
		if (fgets(buff, sizeof(buff), fp) == NULL) {
			ret = -1;
			cverb << vperf << "Did not find Name or PID field in status file." << endl;
			goto out;
		}
		if (!strncmp(buff, "Name:", 5)) {
			char *name = buff + 5;
			while (*name && isspace(*name))
				++name;
			size = strlen(name) - 1;
			// The "Name" field in /proc/pid/status currently only allows for 16 characters,
			// but I'm not going to count on that being stable.  We'll ensure we copy no more
			// than 16 chars  since the comm.comm char array only holds 16.
			size = size > 16 ? 16 : size;
			memcpy(comm.comm, name, size++);
		} else if (memcmp(buff, "Tgid:", 5) == 0) {
			char *tgids = buff + 5;
			while (*tgids && isspace(*tgids))
				++tgids;
			tgid = comm.pid = atoi(tgids);
		}
	}

	comm.header.type = PERF_RECORD_COMM;
	size = align_64bit(size);
	comm.header.size = sizeof(comm) - (sizeof(comm.comm) - size);
	if (tgid != pid) {
		// passed pid must have been a secondary thread
		comm.tid = pid;
		int num = OP_perf_utils::op_write_output(output_fd, &comm, comm.header.size);
		pr->add_to_total(num);
		goto out;
	}

	snprintf(fname, sizeof(fname), "/proc/%d/task", pid);
	tids = opendir(fname);
	if (tids == NULL) {
		// process must have exited
		ret = -1;
		cverb << vperf << "opendir returned NULL" << endl;
		goto out;
	}

	while (!readdir_r(tids, &dirent, &next) && next) {
		char *end;
		pid = strtol(dirent.d_name, &end, 10);
		if (*end)
			continue;

		comm.tid = pid;

		int num = OP_perf_utils::op_write_output(output_fd, &comm, comm.header.size);
		pr->add_to_total(num);
	}
	closedir(tids);

out:
	op_record_process_exec_mmaps(pid, tgid, output_fd, pr);

	fclose(fp);
	if (ret)
		cverb << vperf << "couldn't get app name and tgid for pid "
		      << dec << pid << " from /proc fs." << endl;
	else
		cverb << vperf << "Created COMM event for " << comm.comm << endl;
	return ret;

}

/* Obtain process information for an active process (where the user has
 * passed in a process ID via the --pid option) or all active processes
 * (where system_wide==true).  Then generate the necessary PERF_RECORD_COMM
 * and PERF_RECORD_MMAP entries into the profile data stream.
 */
int OP_perf_utils::op_record_process_info(bool system_wide, pid_t pid, operf_record * pr,
                                          int output_fd)
{
	int ret;
	cverb << vperf << "op_record_process_info" << endl;
	if (!system_wide) {
		ret = _record_one_process_info(pid, pr, output_fd);
	} else {
		char buff[BUFSIZ];
		pid_t tgid = 0;
		size_t size = 0;
		DIR *pids;
		struct dirent dirent, *next;

		pids = opendir("/proc");
		if (pids == NULL) {
			cerr << "Unable to open /proc." << endl;
			return -1;
		}

		while (!readdir_r(pids, &dirent, &next) && next) {
			char *end;
			pid = strtol(dirent.d_name, &end, 10);
			if (((errno == ERANGE && (pid == LONG_MAX || pid == LONG_MIN))
					|| (errno != 0 && pid == 0)) || (end == dirent.d_name)) {
				cverb << vmisc << "/proc entry " << dirent.d_name << " is not a PID" << endl;
				continue;
			}
			if ((ret = _record_one_process_info(pid, pr, output_fd)) < 0)
				break;
		}
		closedir(pids);
	}
	return ret;
}

void OP_perf_utils::op_record_kernel_info(string vmlinux_file, u64 start_addr, u64 end_addr,
                                          int output_fd, operf_record * pr)
{
	struct mmap_event mmap;
	size_t size;
	mmap.header.type = PERF_RECORD_MMAP;
	mmap.header.misc = PERF_RECORD_MISC_KERNEL;
	if (vmlinux_file.empty()) {
		size = strlen( "no_vmlinux") + 1;
		strncpy(mmap.filename, "no_vmlinux", size);
		mmap.start = 0ULL;
		mmap.len = 0ULL;
	} else {
		size = vmlinux_file.length() + 1;
		strncpy(mmap.filename, vmlinux_file.c_str(), size);
		mmap.start = start_addr;
		mmap.len = end_addr - mmap.start;
	}
	size = align_64bit(size);
	mmap.pid = 0;
	mmap.tid = 0;
	mmap.header.size = (sizeof(mmap) -
			(sizeof(mmap.filename) - size));
	int num = op_write_output(output_fd, &mmap, mmap.header.size);
	pr->add_to_total(num);
}

void OP_perf_utils::op_get_kernel_event_data(struct mmap_data *md, operf_record * pr)
{
	struct perf_event_mmap_page *pc = (struct perf_event_mmap_page *)md->base;
	int out_fd = pr->out_fd();

	uint64_t head = pc->data_head;
	// Comment in perf_event.h says "User-space reading the @data_head value should issue
	// an rmb(), on SMP capable platforms, after reading this value."
	rmb();

	uint64_t old = md->prev;
	unsigned char *data = ((unsigned char *)md->base) + pagesize;
	uint64_t size;
	void *buf;
	int64_t diff;

	diff = head - old;
	if (diff < 0) {
		throw runtime_error("ERROR: event buffer wrapped, which should NEVER happen.");
	}

	if (old != head)
		sample_reads++;

	size = head - old;

	if ((old & md->mask) + size != (head & md->mask)) {
		buf = &data[old & md->mask];
		size = md->mask + 1 - (old & md->mask);
		old += size;
		pr->add_to_total(op_write_output(out_fd, buf, size));
	}

	buf = &data[old & md->mask];
	size = head - old;
	old += size;
	pr->add_to_total(op_write_output(out_fd, buf, size));
	md->prev = old;
	pc->data_tail = old;
}


static size_t mmap_size;
static size_t pg_sz;

static int __mmap_trace_file(struct mmap_info & info)
{
	int mmap_prot  = PROT_READ;
	int mmap_flags = MAP_SHARED;

	info.buf = (char *) mmap(NULL, mmap_size, mmap_prot,
			mmap_flags, info.traceFD, info.offset);
	if (info.buf == MAP_FAILED) {
		cerr << "Error: mmap failed with errno:\n\t" << strerror(errno) << endl;
		return -1;
	}
	else {
		cverb << vperf << hex << "mmap with the following parameters" << endl
		      << "\tinfo.head: " << info.head << endl
		      << "\tinfo.offset: " << info.offset << endl;
		return 0;
	}
}

int OP_perf_utils::op_mmap_trace_file(struct mmap_info & info)
{
	u64 shift;
	if (!pg_sz)
		pg_sz = sysconf(_SC_PAGESIZE);
	if (!mmap_size) {
		if (MMAP_WINDOW_SZ > info.file_data_size) {
			mmap_size = info.file_data_size;
		} else {
			mmap_size = MMAP_WINDOW_SZ;
		}
	}
	info.offset = 0;
	info.head = info.file_data_offset;
	shift = pg_sz * (info.head / pg_sz);
	info.offset += shift;
	info.head -= shift;
	return __mmap_trace_file(info);
}

event_t * OP_perf_utils::op_get_perf_event(struct mmap_info & info)
{
	uint32_t size;
	event_t * event;

	if (info.offset + info.head >= info.file_data_offset + info.file_data_size)
		return NULL;

	if (!pg_sz)
		pg_sz = sysconf(_SC_PAGESIZE);

try_again:
	event = (event_t *)(info.buf + info.head);

	if ((mmap_size != info.file_data_size) &&
			(((info.head + sizeof(event->header)) > mmap_size) ||
					(info.head + event->header.size > mmap_size))) {
		int ret;
		u64 shift = pg_sz * (info.head / pg_sz);
		cverb << vperf << "Remapping perf data file" << endl;
		ret = munmap(info.buf, mmap_size);
		if (ret) {
			string errmsg = "Internal error:  munmap of perf data file failed with errno: ";
			errmsg += strerror(errno);
			throw runtime_error(errmsg);
		}

		info.offset += shift;
		info.head -= shift;
		ret = __mmap_trace_file(info);
		if (ret) {
			string errmsg = "Internal error:  mmap of perf data file failed with errno: ";
			errmsg += strerror(errno);
			throw runtime_error(errmsg);
		}
		goto try_again;
	}

	size = event->header.size;

	// The tail end of the operf data file may be zero'ed out, so we assume if we
	// find size==0, we're now in that area of the file, so we're done.
	if (size == 0)
		return NULL;

	info.head += size;
	if (info.offset + info.head >= info.file_data_offset + info.file_data_size)
		return NULL;

	return event;
}


int OP_perf_utils::op_get_next_online_cpu(DIR * dir, struct dirent *entry)
{
#define OFFLINE 0x30
	unsigned int cpu_num;
	char cpu_online_pathname[40];
	int res;
	FILE * online;
	again:
	do {
		entry = readdir(dir);
		if (!entry)
			return -1;
	} while (entry->d_type != DT_DIR);

	res = sscanf(entry->d_name, "cpu%u", &cpu_num);
	if (res <= 0)
		goto again;
	snprintf(cpu_online_pathname, 40, "/sys/devices/system/cpu/cpu%u/online", cpu_num);
	online = fopen(cpu_online_pathname, "r");
	res = fgetc(online);
	fclose(online);
	if (res == OFFLINE)
		goto again;
	else
		return cpu_num;
}

/* Stub for future enhancement. Returns false.
 * Length of build id is BUILD_ID_SIZE
 */
bool OP_perf_utils::get_build_id(char * buildid)
{
	bool ret = false;
	buildid = NULL;
	// TODO:  flesh out this function
	return ret;
}

u64 OP_perf_utils::get_checksum_for_file(string filename)
{
	Elf *elf;
	u64 checksum;
	int fd;

        elf_version(EV_CURRENT);
	fd = open(filename.c_str(), O_RDONLY);
	if (fd < 0)
		return 0;
	elf = elf_begin(fd, ELF_C_READ, NULL);
	checksum = gelf_checksum(elf);
	elf_end(elf);
	close(fd);

	return checksum;
}
