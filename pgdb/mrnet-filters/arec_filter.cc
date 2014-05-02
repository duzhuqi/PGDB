#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <python2.7/Python.h>

#include "mrnet/MRNet.h"
#define pgdbPath "/home/ndryden/PGDB/pgdb/mrnet-filters";
#define PATH_MAX 4096;
#define COMP_TAG 3142;

extern "C" {

	using namespace MRN;

	const char* arec_filter_format_string = "%s";
	PyThreadState* py_state;

	void send_error_packet(unsigned int stream_id, int tag, std::vector<PacketPtr> &packets_out) {
		PacketPtr err_packet(new Packet(stream_id, tag, "%s", "ERROR"));
		packets_out.push_back(err_packet);
	}

	void arec_filter(std::vector<PacketPtr> &packets_in, std::vector<PacketPtr> &packets_out,
					 std::vector<PacketPtr> &packets_out_reverse, void** state, PacketPtr& config_params,
					 const TopologyLocalInfo& topo_info) {
		// Ensure Python is initialized; if it is, this does nothing.
		Py_Initialize();
		// We must serialize access to the Python interpreter.
		// This deals with both the cases where threads are not initialized,
		// which occurs when running on remote comm nodes, and the case where
		// they are already initialized, which occurs on the front-end.
		PyGILState_STATE gstate;
		if (PyEval_ThreadsInitialized() == 0) {
			PyEval_InitThreads();
			py_state = PyThreadState_Get();
		}
		else {
			if (py_state != NULL) {
				PyEval_RestoreThread(py_state);
			}
			else {
				gstate = PyGILState_Ensure();
			}
		}
		// Add the relevant search path to the Python module search path.
		// TODO: Don't hard-code this.
		char * totalPath = char[PATH_MAX];
		strcpy(totalPath, "import sys\n\
sys.path.append('");
		strcat(totalPath, pgdbPath);
		strcat(totalPath, "')\n)");
		PyRun_SimpleString(totalPath);
		// Load the relevant file.
		PyObject* module = PyImport_ImportModule("filter_hook");
		if (module == NULL) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			return;
		}
		// Get the function to call.
		PyObject* filter_func = PyObject_GetAttrString(module, "filter_hook");
		if ((filter_func == NULL) || !PyCallable_Check(filter_func)) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			Py_DECREF(module);
			return;
		}
		// Create the list to pass to the function.
		PyObject* packet_list = PyList_New(packets_in.size());
		if (packet_list == NULL) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			Py_DECREF(module);
			Py_DECREF(filter_func);
			return;
		}
		for (size_t i = 0; i < packets_in.size(); ++i) {
			char* packet_buf;
			PacketPtr cur_packet = packets_in[i];
			
			// Unpack the packet into a buffer.
			if (cur_packet->unpack("%s", &packet_buf) == -1) {
				send_error_packet(packets_in[0]->get_StreamId(),
								  packets_in[0]->get_Tag(),
								  packets_out);
				Py_DECREF(module);
				Py_DECREF(filter_func);
				Py_DECREF(packet_list);
				return;
			}
			// Create a string from the packet data.
			PyObject* unpacked = PyString_FromString(packet_buf);
			if (unpacked == NULL) {
				PyErr_Print();
				send_error_packet(packets_in[0]->get_StreamId(),
								  packets_in[0]->get_Tag(),
								  packets_out);
				Py_DECREF(module);
				Py_DECREF(filter_func);
				Py_DECREF(packet_list);
				return;
			}
			// Unpack allocates a buffer that we need to free.
			free(packet_buf);
			// Add the packet to the list.
			// Note this steals the reference to unpacked.
			if (PyList_SetItem(packet_list, i, unpacked) != 0) {
				PyErr_Print();
				send_error_packet(packets_in[0]->get_StreamId(),
								  packets_in[0]->get_Tag(),
								  packets_out);
				Py_DECREF(module);
				Py_DECREF(filter_func);
				Py_DECREF(packet_list);
				Py_XDECREF(unpacked);
				return;
			}
		}
		// Create the arguments tuple and add the list.
		PyObject* arguments = PyTuple_New(1);
		if (arguments == NULL) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			Py_DECREF(module);
			Py_DECREF(filter_func);
			Py_DECREF(packet_list);
			return;
		}
		if (PyTuple_SetItem(arguments, 0, packet_list) != 0) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			Py_DECREF(module);
			Py_DECREF(filter_func);
			Py_DECREF(packet_list);
			Py_DECREF(arguments);
			return;
		}
		// Call the Python function.
		PyObject* ret_list = PyObject_CallObject(filter_func, arguments);
		if (ret_list == NULL) {
			PyErr_Print();
			send_error_packet(packets_in[0]->get_StreamId(),
							  packets_in[0]->get_Tag(),
							  packets_out);
			Py_DECREF(module);
			Py_DECREF(filter_func);
			Py_DECREF(packet_list);
			Py_DECREF(arguments);
			return;
		}
		if(packets_in[0]->get_Tag() == COMP_TAG){
			char* new_packet_data = (char*) malloc(sizeof(char) * (strlen(python_packet_data) + 1));
			strcpy(new_packet_data, python_packet_data);
			// Construct the new packet.
			PacketPtr new_packet(new Packet(packets_in[0]->get_StreamId(),
											packets_in[0]->get_Tag(),
											"%s",
											new_packet_data));
		}
		// Send it off.
		packets_out.push_back(new_packet);
		// Release all the Python references.
		Py_DECREF(module);
		Py_DECREF(filter_func);
		Py_DECREF(packet_list);
		Py_DECREF(arguments);
		Py_DECREF(ret_list);
		// Release the Python interpreter.
		if (py_state != NULL) {
			py_state = PyEval_SaveThread();
		}
		else {
			PyGILState_Release(gstate);
		}
		/*size_t i;
		for (i = 0; i < packets_in.size(); ++i) {
			packets_out.push_back(packets_in[i]);
		}*/
	}

} /* extern "C" */
