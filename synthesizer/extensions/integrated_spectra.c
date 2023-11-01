/******************************************************************************
 * C extension to calculate integrated SEDs for a galaxy's star particles.
 * Calculates weights on an arbitrary dimensional grid given the mass.
 *****************************************************************************/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <numpy/ndarrayobject.h>
#include <numpy/ndarraytypes.h>

/* Define a macro to handle that bzero is non-standard. */
#define bzero(b, len) (memset((b), '\0', (len)), (void)0)

/**
 * @brief Compute an ndimensional index from a flat index.
 *
 * @param flat_ind: The flattened index to unravel.
 * @param ndim: The number of dimensions for the unraveled index.
 * @param dims: The size of each dimension.
 * @param indices: The output N-dimensional indices.
 */
void get_indices_from_flat(int flat_ind, int ndim, const int *dims,
                           int *indices) {

  /* Loop over indices calculating each one. */
  for (int i = 0; i < ndim; i++) {
    indices[i] = flat_ind % dims[i];
    flat_ind /= dims[i];
  }
}

/**
 * @brief Compute a flat grid index based on the grid dimensions.
 *
 * @param multi_index: An array of N-dimensional indices.
 * @param dims: The length of each dimension.
 * @param ndim: The number of dimensions.
 */
int get_flat_index(const int *multi_index, const int *dims, const int ndims) {
  int index = 0, stride = 1;
  for (int i = ndims - 1; i >= 0; i--) {
    index += stride * multi_index[i];
    stride *= dims[i];
  }

  return index;
}

/**
 * @brief Calculates the mass fractions in each right most grid cell along
 *        each dimension.
 *
 * @param grid_props: An array of the properties along each grid axis.
 * @param part_props: An array of the particle properties, in the same property
 *                    order as grid props.
 * @param p: Index of the current particle.
 * @param ndim: The number of grid dimensions.
 * @param dims: The length of each grid dimension.
 * @param npart: The number of particles in total.
 * @param frac_indices: The array for storing N-dimensional grid indicies.
 * @param fracs: The array for storing the mass fractions. NOTE: The left most
 *               grid cell's mass fraction is simply (1 - frac[dim])
 */
void frac_loop(const double **grid_props, const double **part_props, int p,
               const int ndim, const int *dims, const int npart,
               int *frac_indices, double *fracs) {

  /* Loop over dimensions. */
  for (int dim = 0; dim < ndim; dim++) {

    /* Get this array of grid properties */
    const double *grid_prop = grid_props[dim];

    /* Get this particle property. */
    const double part_val = part_props[dim][p];

    /**************************************************************************
     * Get the cells corresponding to this particle and compute the fraction.
     *************************************************************************/

    /* Define the starting indices. */
    int low = 0, high = dims[dim] - 1;

    /* Here we need to handle if we are outside the range of values. If so
     * there's no point in searching and we return the edge nearest to the
     * value. */
    if (part_val <= grid_prop[low]) {
      low = 0;
      fracs[dim] = 0;
    } else if (part_val > grid_prop[high]) {
      low = dims[dim];
      fracs[dim] = 0;
    } else {

      /* While we don't have a pair of adjacent indices. */
      int diff = high - low;
      while (diff > 1) {

        /* Define the midpoint. */
        int mid = low + floor(diff / 2);

        /* Where is the midpoint relative to the value? */
        if (grid_prop[mid] < part_val) {
          low = mid;
        } else {
          high = mid;
        }

        /* Compute the new range. */
        diff = high - low;
      }

      /* Calculate the fraction. Note, this represents the mass fraction in
       * the high cell. */
      fracs[dim] =
          (part_val - grid_prop[low]) / (grid_prop[high] - grid_prop[low]);
    }

    /* Set these indices. */
    frac_indices[dim] = low;
  }
}

/**
 * @brief This calculates the grid weights in each grid cell.
 *
 * To do this for an N-dimensional array this is done recursively one dimension
 * at a time.
 *
 * @param mass: The mass of the current particle.
 * @param sub_indices: The indices in the 2**ndim subset of grid points being
 *                     computed in the current recursion (entries are 0 or 1).
 * @param frac_indices: The array for storing N-dimensional grid indicies.
 * @param low_indices: The index of the bottom corner grid point.
 * @param weights: The weight of each grid point.
 * @param fracs: The array for storing the mass fractions. NOTE: The left most
 *               grid cell's mass fraction is simply (1 - frac[dim])
 * @param dim: The current dimension in the recursion.
 * @param dims: The length of each grid dimension.
 * @param ndim: The number of grid dimensions.
 */
void recursive_weight_loop(const double mass, short int *sub_indices,
                           int *frac_indices, int *low_indices, double *weights,
                           double *fracs, int dim, const int *dims,
                           const int ndim) {

  /* Are we done yet? */
  if (dim >= ndim) {

    /* Get the flattened index into the grid array. */
    const int weight_ind = get_flat_index(frac_indices, dims, ndim);

    /* Check whether we need a weight in this cell. */
    for (int i = 0; i < ndim; i++) {
      if ((sub_indices[i] == 1 && fracs[i] == 0 && frac_indices[i] == 0) ||
          (sub_indices[i] == 1 && fracs[i] == 0 &&
           frac_indices[i] == dims[i])) {
        return;
      }
    }

    /* Compute the weight. */
    double weight = mass;
    for (int i = 0; i < ndim; i++) {

      /* Account for the fractional contribution in this grid cell. */
      if (sub_indices[i]) {
        weight *= fracs[i];
      } else {
        weight *= (1 - fracs[i]);
      }
    }

    /* And add the weight. */
    weights[weight_ind] += weight;

    /* We're done! */
    return;
  }

  /* Loop over this dimension */
  for (int i = 0; i < ndim; i++) {

    /* Where are we in the sub_array? */
    sub_indices[dim] = i;

    /* Where are we in the grid array? */
    frac_indices[dim] = low_indices[dim] + i;

    /* Recurse... */
    recursive_weight_loop(mass, sub_indices, frac_indices, low_indices, weights,
                          fracs, dim + 1, dims, ndim);
  }
}

/**
 * @brief Computes an integrated SED for a collection of particles.
 *
 * @param np_grid_spectra: The SPS spectra array.
 * @param grid_tuple: The tuple containing arrays of grid axis properties.
 * @param part_tuple: The tuple of particle property arrays (in the same order
 *                    as grid_tuple).
 * @param np_part_mass: The particle mass array.
 * @param fesc: The escape fraction.
 * @param np_ndims: The size of each grid axis.
 * @param ndim: The number of grid axes.
 * @param npart: The number of particles.
 * @param nlam: The number of wavelength elements.
 */
PyObject *compute_integrated_sed(PyObject *self, PyObject *args) {

  const int ndim;
  const int npart, nlam;
  const double fesc;
  const PyObject *grid_tuple, *part_tuple;
  const PyArrayObject *np_grid_spectra;
  const PyArrayObject *np_part_mass, *np_ndims;

  if (!PyArg_ParseTuple(args, "OOOOdOiii", &np_grid_spectra, &grid_tuple,
                        &part_tuple, &np_part_mass, &fesc, &np_ndims, &ndim,
                        &npart, &nlam))
    return NULL;

  /* Quick check to make sure our inputs are valid. */
  if (ndim == 0)
    return NULL;
  if (npart == 0)
    return NULL;
  if (nlam == 0)
    return NULL;

  /* Extract a pointer to the spectra grids */
  const double *grid_spectra = PyArray_DATA(np_grid_spectra);

  /* Set up arrays to hold the SEDs themselves. */
  double *spectra = malloc(nlam * sizeof(double));
  bzero(spectra, nlam * sizeof(double));

  /* Extract a pointer to the grid dims */
  const int *dims = PyArray_DATA(np_ndims);

  /* Extract a pointer to the particle masses. */
  const double *part_mass = PyArray_DATA(np_part_mass);

  /* Compute the number of weights we need. Adding on a buffer for
   * accurate casting*/
  const int nweights = pow(2, ndim) + 0.1;

  /* Allocate a single array for grid properties*/
  int nprops = 0;
  for (int dim = 0; dim < ndim; dim++)
    nprops += dims[dim];
  const double **grid_props = malloc(nprops * sizeof(double *));

  /* How many grid elements are there? (excluding wavelength axis)*/
  int grid_size = 1;
  for (int dim = 0; dim < ndim; dim++)
    grid_size *= dims[dim];

  /* Allocate an array to hold the grid weights. */
  double *grid_weights = malloc(grid_size * sizeof(double));
  bzero(grid_weights, grid_size * sizeof(double));

  /* Unpack the grid property arrays into a single contiguous array. */
  for (int idim = 0; idim < ndim; idim++) {

    /* Extract the data from the numpy array. */
    const PyArrayObject *np_grid_arr = PyTuple_GetItem(grid_tuple, idim);
    const double *grid_arr = PyArray_DATA(np_grid_arr);

    /* Assign this data to the property array. */
    grid_props[idim] = grid_arr;
  }

  /* Allocate a single array for particle properties. */
  const double **part_props = malloc(npart * ndim * sizeof(double *));

  /* Unpack the particle property arrays into a single contiguous array. */
  for (int idim = 0; idim < ndim; idim++) {

    /* Extract the data from the numpy array. */
    const PyArrayObject *np_part_arr = PyTuple_GetItem(part_tuple, idim);
    const double *part_arr = PyArray_DATA(np_part_arr);

    /* Assign this data to the property array. */
    part_props[idim] = part_arr;
  }

  /* Set up arrays to store grid indices for the weights, mass fractions
   * and indices.
   * NOTE: the wavelength index on frac_indices is always 0. */
  double fracs[ndim];
  int frac_indices[ndim + 1];
  int low_indices[ndim + 1];
  short int sub_indices[ndim];

  /* Loop over particles. */
  for (int p = 0; p < npart; p++) {

    /* Reset fraction and indices arrays to zero. */
    for (int ind = 0; ind < ndim; ind++) {
      fracs[ind] = 0;
      sub_indices[ind] = 0;
    }
    for (int ind = 0; ind < ndim + 1; ind++) {
      frac_indices[ind] = 0;
      low_indices[ind] = 0;
    }

    /* Get this particle's mass. */
    const double mass = part_mass[p];

    /* Compute grid indices and the mass faction in each grid cell. */
    frac_loop(grid_props, part_props, p, ndim, dims, npart, frac_indices,
              fracs);

    /* Make a copy of the indices of the fraction to avoid modification
     * during recursion. */
    for (int ind = 0; ind < ndim + 1; ind++) {
      low_indices[ind] = frac_indices[ind];
    }

    /* Finally, compute the weights for this particle. */
    recursive_weight_loop(mass, sub_indices, frac_indices, low_indices,
                          grid_weights, fracs, /*dim*/ 0, dims, ndim);

  } /* Loop over particles. */

  /* Loop over grid cells. */
  for (int grid_ind = 0; grid_ind < grid_size; grid_ind++) {

    /* Get the weight. */
    const double weight = grid_weights[grid_ind];

    /* Skip zero weight cells. */
    if (weight <= 0)
      continue;

    /* Get the spectra ind. */
    int unraveled_ind[ndim + 1];
    get_indices_from_flat(grid_ind, ndim, dims, unraveled_ind);
    unraveled_ind[ndim] = 0;
    int spectra_ind = get_flat_index(unraveled_ind, dims, ndim + 1);

    /* Add this grid cell's contribution to the spectra */
    for (int ilam = 0; ilam < nlam; ilam++) {

      /* Add the contribution to this wavelength. */
      spectra[ilam] += grid_spectra[spectra_ind + ilam] * (1 - fesc) * weight;
    }
  }

  /* Clean up memory! */
  free(grid_weights);
  free(part_props);
  free(grid_props);

  /* Reconstruct the python array to return. */
  npy_intp np_dims[1] = {
      nlam,
  };
  PyArrayObject *out_spectra = (PyArrayObject *)PyArray_SimpleNewFromData(
      1, np_dims, NPY_FLOAT64, spectra);

  return Py_BuildValue("N", out_spectra);
}

/* Below is all the gubbins needed to make the module importable in Python. */
static PyMethodDef SedMethods[] = {
    {"compute_integrated_sed", compute_integrated_sed, METH_VARARGS,
     "Method for calculating integrated intrinsic spectra."},
    {NULL, NULL, 0, NULL}};

/* Make this importable. */
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "make_sed",                              /* m_name */
    "A module to calculate integrated seds", /* m_doc */
    -1,                                      /* m_size */
    SedMethods,                              /* m_methods */
    NULL,                                    /* m_reload */
    NULL,                                    /* m_traverse */
    NULL,                                    /* m_clear */
    NULL,                                    /* m_free */
};

PyMODINIT_FUNC PyInit_integrated_spectra(void) {
  PyObject *m = PyModule_Create(&moduledef);
  import_array();
  return m;
}
