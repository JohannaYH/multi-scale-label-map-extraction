/*
------------------------------------------------------------------------------ -
MIT License

Copyright(c) 2016 Yale University

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

This is based on the source for the paper

"Multi-scale label-map extraction for texture synthesis"
in the ACM Transactions on Graphics(TOG) - Proceedings of ACM SIGGRAPH 2016,
Volume 35 Issue 4, July 2016
by
Lockerman, Y.D., Sauvage, B., Allegre,
R., Dischler, J.M., Dorsey, J.and Rushmeier, H.

http://graphics.cs.yale.edu/site/publications/multi-scale-label-map-extraction-texture-synthesis

If you find it useful, please consider giving us credit or citing our paper.
------------------------------------------------------------------------------ -

@author : Yitzchak David Lockerman
*/

#include "region_io.h"
#include "region_map.h"
#include "matio.h"
#include <stdint.h>
#include <algorithm>  
#include <iostream>

size_t get_total_element_count(matvar_t* arry)
{
	size_t number_of_element = 1;
	for (int dim = 0; dim < arry->rank; dim++)
		number_of_element *= arry->dims[dim];

	return number_of_element;
}



matvar_raii::matvar_raii(matvar_t* ptr) : std::unique_ptr<matvar_t, void (*)(matvar_t *)>(ptr, Mat_VarFree)
{}

matvar_raii::operator matvar_t* ()
{
	return get();
}



mat_raii::mat_raii(_mat_t* ptr) : std::unique_ptr<_mat_t, int(*)(_mat_t *)>(ptr, Mat_Close)
{}

mat_raii::operator _mat_t* ()
{
	return get();
}


/*
Returns a field within a struct_array. Note that the item should not be freed. 
*/
matvar_t* get_struct_field(matvar_t* struct_array, std::string name, bool optional=false)
{
	matvar_t* out = Mat_VarGetStructField(struct_array, (void*)name.c_str(), MAT_BY_NAME, 0);

	if (out == NULL || out->nbytes <= 0)
	{
		if (optional)
			return NULL;
		else
			throw std::exception( ("Struct is missing expected element: " + name).c_str());
	}

	return out;
}

/*
Loads a region (Superpixel or label) from a struct_array 
*/
CompoundRegion load_region(matvar_t* struct_array)
{
	if (struct_array->data_type != MAT_T_STRUCT )
	{
		throw std::exception("cells should have struct array");
	}


	//load the list of atomic superpixels.
	//This will need to be a array of 32 or 64 bit integers.
	matvar_t* list_of_atomic_superpixels = get_struct_field(struct_array, "list_of_atomic_superpixels");

	size_t number_of_atomic_superpixels = list_of_atomic_superpixels->nbytes / list_of_atomic_superpixels->data_size;
	CompoundRegion out;
	out.atomic_superpixels.resize(number_of_atomic_superpixels);

	if (list_of_atomic_superpixels->data_type == MAT_T_INT32)
	{
		int32_t* atomic_list = (int32_t*)list_of_atomic_superpixels->data;
		std::copy(atomic_list, atomic_list + number_of_atomic_superpixels, out.atomic_superpixels.begin());
	}
	else if (list_of_atomic_superpixels->data_type == MAT_T_INT64)
	{
		int64_t* atomic_list = (int64_t*)list_of_atomic_superpixels->data;
		std::copy(atomic_list, atomic_list + number_of_atomic_superpixels, out.atomic_superpixels.begin());
	}
	else
	{
		throw std::exception("list_of_atomic_superpixels has unknown type");
	}

	return out;
}

/*
This method loads a region from the tree format used in our file.
*/
std::vector<HierarchicalRegionPtr> load_region_node_from_cell_array(matvar_t* cell_array)
{
	if (cell_array->data_type != MAT_T_CELL || cell_array->nbytes < 1)
		throw std::exception("Invalid tree data structure");

	size_t number_of_cells = get_total_element_count(cell_array);

	matvar_t **all_cells = Mat_VarGetCellsLinear(cell_array, 0, 1, (int)number_of_cells);

	if (all_cells == NULL)
		throw std::exception("Invalid tree data structure");

	std::vector<HierarchicalRegionPtr> output(number_of_cells);
	for (int i = 0; i < number_of_cells; i++)
	{
		try
		{
			//Find the elements of intrest and use them
			output[i] = HierarchicalRegionPtr(new HierarchicalRegion());
			(*(std::dynamic_pointer_cast<CompoundRegion>(output[i]))) = load_region(all_cells[i]);

			//load the scale. This will need to be a float or double. 
			matvar_t* scale = get_struct_field(all_cells[i], "scale");

			if (scale->data_type == MAT_T_SINGLE)
			{
				output[i]->scale = *((float*)scale->data);
			}
			else if (scale->data_type == MAT_T_DOUBLE)
			{
				output[i]->scale = (float)*((double*)scale->data);
			}
			else
			{
				throw std::exception("Scale has unknown type");
			}

			//Recursivly load the subregions
			matvar_t * children = get_struct_field(all_cells[i], "children", true);

			if (children != NULL)
				output[i]->children_node = load_region_node_from_cell_array(children);
		}
		catch (...)
		{
			free(all_cells);
			throw;
		}
	}


	free(all_cells);
	return output;
}


image_size load_image_size(matvar_t *image_shape)
{
	image_size output;


	if (image_shape == NULL)
		throw std::exception("File dose not include image_shape.");

	if (image_shape->rank != 2 || image_shape->isComplex ||
		image_shape->dims[0] != 1 || image_shape->dims[1] != 3)
		throw std::exception("image_shape is invalid.  ");


	if (image_shape->data_type == MAT_T_INT64)
	{
		int64_t* image_shape_data = (int64_t*)image_shape->data;
		output.rows = image_shape_data[0];
		output.cols = image_shape_data[1];
		output.stride = image_shape_data[2];
	}
	else if (image_shape->data_type == MAT_T_INT32)
	{
		int32_t* image_shape_data = (int32_t*)image_shape->data;
		output.rows = image_shape_data[0];
		output.cols = image_shape_data[1];
		output.stride = image_shape_data[2];
	}
	else
	{
		throw std::exception("image_shape has unknown type.");
	}

	return output;
}





template<typename T>
std::valarray<size_t> load_rle_data(T* data_array, size_t number_of_ellements, const image_size& size)
{
	std::valarray<size_t> slic_indexes(size.rows*size.cols);

	auto itter = begin(slic_indexes);

	for (size_t elid = 0; elid < number_of_ellements; elid++)
	{
		T run_lenght = data_array[elid];
		T value = data_array[number_of_ellements + elid];

		itter = std::fill_n(itter, run_lenght, value);
	}

	return slic_indexes;

}

std::valarray<size_t> load_atomic_regions_from_rle(matvar_t *atomic_region_rle, const image_size size)
{
	if (atomic_region_rle == NULL)
		throw std::exception("File does not include atomic_SLIC_rle");


	if (atomic_region_rle->rank != 2 || atomic_region_rle->isComplex ||
		atomic_region_rle->dims[1] != 2)
			throw std::exception("atomic_SLIC_rle is invalid.");

	if (atomic_region_rle->data_type == MAT_T_INT64)
		return load_rle_data((int64_t*)atomic_region_rle->data, atomic_region_rle->dims[0], size);
	else if (atomic_region_rle->data_type == MAT_T_INT32)
		return load_rle_data((int32_t*)atomic_region_rle->data, atomic_region_rle->dims[0], size);
	else
		throw std::exception("atomic_SLIC_rle has unknown type.");
}