#include "ion_util.h"

int server_is_ion(int srv_id, int ion_modulus)
{
	if(srv_id % ion_modulus == 0)
		return 1;
	else
		return 0;
}


