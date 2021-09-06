#include <opae/cxx/core/handle.h>
#include <opae/cxx/core/properties.h>
#include <opae/cxx/core/shared_buffer.h>
#include <opae/cxx/core/token.h>
#include <opae/cxx/core/version.h>
#include <uuid/uuid.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include "afu_json_info.h"
#include "hardposit.h"
#include "config.h"
using namespace opae::fpga::types;


static handle::ptr_t accel = nullptr;
static uint16_t counter = 0;
shared_buffer::ptr_t req;
shared_buffer::ptr_t result;
uint32_t req_q_pointer = 0;
uint32_t result_q_pointer = 0;
Hardposit* result_track[NUM_ENTRIES];

void init_accel(){
	if(!accel){
		properties::ptr_t filter = properties::get();
		filter->guid.parse(AFU_ACCEL_UUID);
		filter->type = FPGA_ACCELERATOR;

		std::vector<token::ptr_t> tokens = token::enumerate({filter});

		// assert we have found at least one
		if (tokens.size() < 1) {
			std::cerr << "accelerator not found\n";
			return -1;
		}
		token::ptr_t tok = tokens[0];
		accel = handle::open(tok, FPGA_OPEN_SHARED);
		req = shared_buffer::allocate(accel, NUM_ENTRIES*READ_GRANULATIRY);
		result = shared_buffer::allocate(accel, NUM_ENTRIES*WRITE_GRANULATIRY);
	  	std::fill_n(req->c_type(), NUM_ENTRIES*READ_GRANULATIRY, 0);
	  	std::fill_n(result->c_type(), NUM_ENTRIES*WRITE_GRANULATIRY, 0);

		// open accelerator and map MMIO
		accel->reset();

		uint64_t read_setup = 0;
		read_setup += ((req->io_address())>>6);
		read_setup += (READ_GRANULATIRY << 42);

		uint64_t write_setup = 0;
		write_setup += ((result->io_address())>>6);
		write_setup += (WRITE_GRANULATIRY << 42);
		
		accel->write_csr64(0, read_setup);
		accel->write_csr64(8, write_setup);
	}
}

/*
 Hardposit:print_val
 print the value of the hardposit
*/
void Hardposit::print_val(){
	std::cout << "Hardposit: "<< this->get_val() << std::endl;
}

uint32_t Hardposit::get_val(){
	if(!this->valid){
		while(!this->ptr->flags){}
		this->valid = true;
		std::memcpy(&this->val, &this->ptr->reuslt, sizeof(uint32_t));
		this->ptr->flags = 0;
	}
	return this->val;
}

Hardposit::Hardposit(uint32_t in_val){
	this->val = in_val;
	this->valid = true;
	this->ptr = nullptr;
	this->location = 0;
	this->counter = 0;
}

Hardposit Hardposit::compute(Hardposit const &obj1, Hardposit const &obj2, Inst inst, bool mode){

	Operand ops[3];
	Hardposit ret;
	if(this->ptr && (counter-this->counter) <= 8){
		ops[0].addr = this->location;
		ops[0].addr_mode= 1;
	}else{

		ops[0].addr = req_q_pointer;
		ops[0].addr_mode= 2;
		uint32_t res = this->get_val();
		mempcpy(((void*)req->c_type() + sizeof(uint32_t)*req_q_pointer), &res, sizeof(uint32_t));
		req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;

	}

	if(obj1.ptr && (counter-obj1.counter) <= 8){
		ops[1].addr = obj1.location;
		ops[1].addr_mode= 1;
	}else{
		ops[1].addr = req_q_pointer;
		ops[1].addr_mode= 2;
		uint32_t res = obj1.get_val();
		mempcpy(((void*)req->c_type() + sizeof(uint32_t)*req_q_pointer), &res, sizeof(uint32_t));		
		req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;

	}
	if(inst == Inst::FMA){
		if(obj2.ptr && (counter-obj2.counter) <= 8){
			ops[2].addr = obj2.location;
			ops[2].addr_mode= 1;
		}else{
			ops[2].addr = req_q_pointer;
			ops[2].addr_mode= 2;
			uint32_t res = obj2.get_val();
			mempcpy(((void*)req->c_type() + sizeof(uint32_t)*req_q_pointer), &res, sizeof(uint32_t));		
			req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;
			req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;
		}
	}else{
		ops[2].addr_mode= 0;
	}
	Hardposit* res = *(result_track+result_q_pointer);
	if( res!= nullptr){
		if(!res->valid){
			res->get_val();
		}
	}

	ret.valid = false;

	ret.ptr = (Result*)((void*)result->c_type() +result_q_pointer*WRITE_GRANULATIRY);
	ret.counter = counter;
	ret.location = counter % FPGA_ENTRIES;
	uint64_t write_req = 0;
	write_req += result_q_pointer;

	mempcpy((uint8_t*)&write_req+1, ops, 3*sizeof(Operand));

	uint8_t insmod =  inst + mode << 3;
	write_req += insmod << 56;
	result_q_pointer  = (result_q_pointer+1) % NUM_ENTRIES;
	counter ++;
	std::cout<<"valid"<<ret.valid<<std::endl;
	std::cout<<"flags"<<ret.ptr->flags<<std::endl;
	accel->write_csr64(16, write_req);
	return ret;
}

Hardposit_cmp Hardposit::compute_cmp(Hardposit const &obj, Inst inst, bool mode){
	Operand ops[2];
	Hardposit_cmp ret;
	if(this->ptr && (counter-this->counter) <= 8){
		ops[0].addr = this->location;
		ops[0].addr_mode= 1;
	}else{
		ops[0].addr = req_q_pointer;
		ops[0].addr_mode= 2;
		mempcpy(((void*)req->c_type() + sizeof(uint32_t)*req_q_pointer), this->get_val(), sizeof(uint32_t));
		req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;

	}

	if(obj.ptr && (counter-obj.counter) <= 8){
		ops[1].addr = obj.location;
		ops[1].addr_mode= 1;
	}else{
		ops[1].addr = req_q_pointer;
		ops[1].addr_mode= 2;
		mempcpy(req->c_type() + sizeof(uint32_t)*req_q_pointer, obj.get_val(), sizeof(uint32_t));
		req_q_pointer = (req_q_pointer + 1) % NUM_ENTRIES;

	}
	ret.valid = false;
	ret.ptr = (Result*)(result.get()+result_q_pointer*WRITE_GRANULATIRY);
	uint64_t write_req = 0;
	write_req += result_q_pointer;
	mempcpy((uint8_t*)&write_req+1, ops, 2*sizeof(Operand));
	uint8_t insmod =  inst + mode << 3;
	write_req += insmod << 56;
	result_q_pointer  = (result_q_pointer+1) % NUM_ENTRIES;
	counter ++;
	accel->write_csr64(16, write_req);
	return ret;
}

Hardposit_cmp::~Hardposit_cmp(){
	if(this->ptr && !this->valid){
		this->ptr->flags = 0;
	}
}

Hardposit::~Hardposit(){
	if(this->ptr && !this->valid){
		this->ptr->flags = 0;
	}
}



Hardposit Hardposit::operator + (Hardposit const &obj){
	return this->compute(obj, Hardposit(), ADDSUB, false);
}
Hardposit Hardposit::operator - (Hardposit const &obj){
	return this->compute(obj, Hardposit(), ADDSUB, true);
}
Hardposit Hardposit::operator / (Hardposit const &obj){
	return this->compute(obj, Hardposit(), SQRTDIV, false);
}
Hardposit Hardposit::operator * (Hardposit const &obj){
	return this->compute(obj, Hardposit(), MUL, false);

}
Hardposit_cmp Hardposit::operator < (Hardposit const &obj){
	Hardposit_cmp result = this->compute_cmp(obj, CMP, false);
	result.type = LT;
	return result;
}
Hardposit_cmp Hardposit::operator > (Hardposit const &obj){
	Hardposit_cmp result = this->compute_cmp(obj, CMP, false);
	result.type = GT;
	return result;
}
Hardposit_cmp Hardposit::operator == (Hardposit const &obj){
	Hardposit_cmp result = this->compute_cmp(obj, CMP, false);
	result.type = EQ;
	return result;
}
Hardposit Hardposit::sqrt(Hardposit const &obj){
	return this->compute(obj, Hardposit(), SQRTDIV, true);
}
Hardposit Hardposit::FMA(Hardposit const &obj1, Hardposit const &obj2)
{
	return this->compute(obj1, obj2, Inst::FMA, false);
}
void Hardposit::operator = (Hardposit const &obj){
	this->val = obj.val;
	this->valid = obj.valid;
	this->ptr = obj.ptr;
	this->location = obj.location;
	this->counter = obj.counter;
}


void Hardposit_cmp::print_val(){
	std::cout << "Hardposit_cmp: "<< this->get_val() << std::endl;
}
bool Hardposit_cmp::get_val(){
	if(!this->valid){
		while(!this->ptr->flags){}
		this->valid = true;
		switch (this->type){
			case LT:
				this->val = this->ptr->flags & 4;
				break;
			case EQ:
				this->val = this->ptr->flags & 2;
				break;
			case GT:
				this->val = this->ptr->flags & 1;
			default:
				break;
		}
		this->ptr->flags = 0;
	}
	return this->val;
}
Hardposit_cmp::Hardposit_cmp(bool in_val){
	this->val = in_val;
	this->valid = true;
	this->ptr = nullptr;
}

void Hardposit_cmp::operator = (Hardposit_cmp const &obj){
	this->val = obj.val;
	this->valid = obj.valid;
	this->ptr = obj.ptr;
	this->type = obj.type;
}