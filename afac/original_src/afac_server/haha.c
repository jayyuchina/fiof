/* original read for we used 1k 2k item*/
{	
	/*3 related to RADIX_GRA*/
	do
	{	
		if((start_per_page >= ptr->offset) &&(end_per_page <= (ptr->offset + ptr->length)))
			cur_ptr = ptr;
		ptr = ptr->radix_next;
			
	}while (ptr)
		if(cur_ptr != NULL)
	{
		/* cache hit in this page*/
		memcpy(data_buffer, cur_ptr->data+(start_per_page - cur_ptr->offset), end_per_page - start_per_page);
	} else {	
		/* need to read data from (start_per_page/RADIX_GRA)*RADIX_GRA  to end_per_page, exactly a full page*/
		ptr = it;
		it = do_item_alloc(end_per_page - RADIX_GRA, RADIX_GRA ,RADIX_GRA);
		do {
			memcpy(it->data + (ptr->offset - (end_per_page - RADIX_GRA)), ptr->data, ptr->length);
			radix_tree_delete(file_head->root,start_per_page/RADIX_GRA);
			cur_ptr = ptr;
			ptr = ptr->radix_next;
			do_item_remove(cur_ptr);
		} while(ptr)
		radix_tree_insert(file_head->root,start_per_page/RADIX_GRA,it);
		do_item_link(it, file_head);
		memcpy(data_buffer, it->data + (start_per_page - (end_per_page-RADIX_GRA)), end_per_page - start_per_page);
	}
}

