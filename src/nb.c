/*
  Copyright Simon Mitternacht 2013-2015.

  This file is part of FreeSASA.

  FreeSASA is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FreeSASA is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License forr more details.

  You should have received a copy of the GNU General Public License
  along with FreeSASA.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "freesasa.h"
#include "nb.h"

#define NB_CHUNK 32

typedef struct freesasa_nb_element freesasa_nb_element;

typedef struct cell cell;
struct cell {
    cell *nb[17]; //! includes self, only forward neighbors
    int *atom; //! indices of the atoms/coordinates in a cell
    int n_nb; //! number of neighbors to cell
    int n_atoms; //! number of atoms in cell
};

//! Verlet cell lists
typedef struct cell_list {
    cell *cell; //! the cells
    int n; //! number of cells
    int nx, ny, nz; //! number of cells along each axis
    double d; //! cell size
    double x_max, x_min;
    double y_max, y_min;
    double z_max, z_min;
} cell_list;

extern int freesasa_fail(const char *format, ...);
extern int freesasa_warn(const char *format, ...);

//! Finds the bounds of the cell list and writes them to the provided cell list
static void cell_list_bounds(cell_list *c, 
                             const freesasa_coord *coord)
{
    const int n = freesasa_coord_n(coord);
    double d = c->d;
    const double *v = freesasa_coord_i(coord,0);
    double x=v[0],X=v[0],y=v[1],Y=v[1],z=v[2],Z=v[2];
    for (int i = 1; i < n; ++i) {
        v = freesasa_coord_i(coord,i);
        x = fmin(v[0],x);
        X = fmax(v[0],X);
        y = fmin(v[1],y);
        Y = fmax(v[1],Y);
        z = fmin(v[2],z);
        Z = fmax(v[2],Z);
    }
    c->x_min = x - d/2.;
    c->x_max = X + d/2.;
    c->y_min = y - d/2.;
    c->y_max = Y + d/2.;
    c->z_min = z - d/2.;
    c->z_max = Z + d/2.;
    c->nx = ceil((c->x_max - c->x_min)/d);
    c->ny = ceil((c->y_max - c->y_min)/d);
    c->nz = ceil((c->z_max - c->z_min)/d);
    c->n = c->nx*c->ny*c->nz;
}

//! find the given
static inline int cell_index(const cell_list *c,
                             int ix, int iy, int iz)
{
    assert(ix >= 0 && ix < c->nx);
    assert(iy >= 0 && iy < c->ny);
    return ix + c->nx*(iy + c->ny*iz);
}

//! Fill the neighbor list for a given cell, only "forward" neighbors considered
static void fill_nb(cell_list *c,
                    int ix, int iy, int iz)
{
    cell *cell = &c->cell[cell_index(c,ix,iy,iz)];
    int n = 0;
    int xmin = ix > 0 ? ix - 1 : 0;
    int xmax = ix < c->nx - 1 ? ix + 1 : ix;
    int ymin = iy > 0 ? iy - 1 : 0;
    int ymax = iy < c->ny - 1 ? iy + 1 : iy;
    int zmin = iz > 0 ? iz - 1 : 0;
    int zmax = iz < c->nz - 1 ? iz + 1 : iz;
    for (int i = xmin; i <= xmax; ++i) {
        for (int j = ymin; j <= ymax; ++j) {
            for (int k = zmin; k <= zmax; ++k) {
                /* Scalar product between (i-ix,j-iy,k-iz) and (1,1,1) should
                   be non-negative. Using only forward neighbors means
                   there's no double counting when comparing cells */
                if (i-ix+j-iy+k-iz >= 0) { 
                    cell->nb[n] = &c->cell[cell_index(c,i,j,k)];
                    ++n;
                }
            }
        }
    }
    cell->n_nb = n;
    assert(n > 0);
}

//! find neighbors to all cells
static void get_nb(cell_list *c)
{
    for (int ix = 0; ix < c->nx; ++ix) {
        for (int iy = 0; iy < c->ny; ++iy) {
            for (int iz = 0; iz < c->nz; ++iz) {
                fill_nb(c,ix,iy,iz);
            }
        }
    }
}
//! Get the cell index of a given atom
static int coord2cell_index(const cell_list *c, const double *xyz)
{
    double d = c->d;
    int ix = (int)((xyz[0] - c->x_min)/d);
    int iy = (int)((xyz[1] - c->y_min)/d);
    int iz = (int)((xyz[2] - c->z_min)/d);
    return cell_index(c,ix,iy,iz);
}
//! Assigns cells to each coordinate
static void fill_cells(cell_list *c, const freesasa_coord *coord)
{
    const double d = c->d;
    const int nx = c->nx, ny = c->ny, nz = c->nz;
    for (int i = 0; i < c->n; ++i) {
        c->cell[i].n_atoms = 0;
    }
    for (int i = 0; i < freesasa_coord_n(coord); ++i) {
        const double *v = freesasa_coord_i(coord,i);
        cell *cell;
        cell = &c->cell[coord2cell_index(c,v)];
        ++cell->n_atoms;
        cell->atom = realloc(cell->atom,sizeof(int)*cell->n_atoms);
        assert(cell->atom);
        cell->atom[cell->n_atoms-1] = i;
        
    }
}

/**
    Frees an object created by cell_list_new().
*/
static void cell_list_free(cell_list *c)
{
    if (c) {
        for (int i = 0; i < c->n; ++i) free(c->cell[i].atom);
        free(c->cell);
        free(c);
    }
}

/**
    Creates a cell list with provided cell-size assigning cells to
    each of the provided coordinates. The created cell list should be
    freed using cell_list_free().
 */
static cell_list* cell_list_new(double cell_size,
                                const freesasa_coord *coord)
{
    assert(cell_size > 0);
    assert(coord);

    cell_list *c = malloc(sizeof(cell_list));

    c->d = cell_size;
    cell_list_bounds(c,coord);
    c->cell = malloc(sizeof(cell)*c->n);
    for (int i = 0; i < c->n; ++i) c->cell[i].atom = NULL;
    fill_cells(c,coord);
    get_nb(c);
    return c;
}

//! assumes max value in a is positive
static double max_array(const double *a,int n)
{
    double max = 0;
    for (int i = 0; i < n; ++i) {
        max = fmax(a[i],max);
    }
    return max;
}

//! allocate memory for ::freesasa_nb object
static freesasa_nb *freesasa_nb_alloc(int n)
{
    assert(n > 0);
    freesasa_nb *adj = malloc(sizeof(freesasa_nb));

    adj->n = n;
    adj->nn = malloc(sizeof(int)*n);
    adj->nb = malloc(sizeof(int*)*n);
    adj->nb_xyd = malloc(sizeof(double *)*n);
    adj->nb_xd = malloc(sizeof(double *)*n);
    adj->nb_yd = malloc(sizeof(double *)*n);
    adj->capacity = malloc(sizeof(int *)*n);
    
    for (int i=0; i < n; ++i) {
        adj->nn[i] = 0;
        adj->capacity[i] = NB_CHUNK;
        adj->nb[i] = malloc(sizeof(int)*NB_CHUNK);
        adj->nb_xyd[i] = malloc(sizeof(double)*NB_CHUNK);
        adj->nb_xd[i] = malloc(sizeof(double)*NB_CHUNK);
        adj->nb_yd[i] = malloc(sizeof(double)*NB_CHUNK);
    }
    return adj;
}

void freesasa_nb_free(freesasa_nb *adj)
{
    if (adj != NULL) {
        for (int i = 0; i < adj->n; ++i) {
            free(adj->nb[i]);
            free(adj->nb_xyd[i]);
            free(adj->nb_xd[i]);
            free(adj->nb_yd[i]);
        }
        free(adj->nn);
        free(adj->nb);
        free(adj->nb_xyd);
        free(adj->nb_xd);
        free(adj->nb_yd);
        free(adj->capacity);
        free(adj);
    }
}

//! increases sizes of arrays when they cross a threshold
static void chunk_up(int *capacity, 
                     int nni, 
                     int **nbi, 
                     double **xydi, 
                     double **xdi, 
                     double **ydi) 
{
    if (nni > *capacity) {
        *capacity += NB_CHUNK;
        *nbi = realloc(*nbi,sizeof(int)*(*capacity)); 
        *xydi = realloc(*xydi,sizeof(double)*(*capacity));
        *xdi = realloc(*xdi,sizeof(double)*(*capacity));
        *ydi = realloc(*ydi,sizeof(double)*(*capacity));
        assert(*nbi); assert(*xydi); assert(*xdi); assert(*ydi);
    }
}

/**
    Assumes the coordinates i and j have been determined to be
    neighbors and adds them both to the provided nb lists,
    symmetrically.
*/
static void nb_add_pair(freesasa_nb *adj,int i, int j,
                            double dx, double dy)
{
    assert(i != j);

    int **nb = adj->nb;
    int *nn = adj->nn;
    double **nb_xyd = adj->nb_xyd;
    double **nb_xd = adj->nb_xd;
    double **nb_yd = adj->nb_yd;
    double d;

    ++nn[i]; ++nn[j];

    chunk_up(&(adj->capacity[i]), nn[i], &nb[i], &nb_xyd[i], &nb_xd[i], &nb_yd[i]);
    chunk_up(&(adj->capacity[j]), nn[j], &nb[j], &nb_xyd[j], &nb_xd[j], &nb_yd[j]);

    nb[i][nn[i]-1] = j;
    nb[j][nn[j]-1] = i;

    d = sqrt(dx*dx+dy*dy);

    nb_xyd[i][nn[i]-1] = d;
    nb_xyd[j][nn[j]-1] = d;
    
    nb_xd[i][nn[i]-1] = dx;
    nb_xd[j][nn[j]-1] = -dx;
    nb_yd[i][nn[i]-1] = dy;
    nb_yd[j][nn[j]-1] = -dy;
}

/**
    Fills the nb list for all contacts between coordinates
    belonging to the cells ci and cj. Handles the case ci == cj
    correctly.
*/
static void nb_calc_cell_pair(freesasa_nb *adj,
                              const freesasa_coord* coord,
                              const double *radii,
                              const cell *ci,
                              const cell *cj)
{
    const double *v = freesasa_coord_all(coord);
    double ri, rj, xi, yi, zi, xj, yj, zj,
        dx, dy, dz, cut2;
    int count_neighbors[ci->n_atoms];
    int i,j,ia,ja;
    
    for (i = 0; i < ci->n_atoms; ++i) {
        ia = ci->atom[i];
        ri = radii[ia];
        xi = v[ia*3]; yi = v[ia*3+1]; zi = v[ia*3+2];
        if (ci == cj) j = i+1;
        else j = 0;
        // the following loop is performance critical
        for (; j < cj->n_atoms; ++j) {
            ja = cj->atom[j];
            assert (ia != ja);
            rj = radii[ja];
            xj = v[ja*3]; yj = v[ja*3+1]; zj = v[ja*3+2];
            cut2 = (ri+rj)*(ri+rj);
            if ((xj-xi)*(xj-xi) > cut2 ||
                (yj-yi)*(yj-yi) > cut2 ||
                (zj-zi)*(zj-zi) > cut2) {
                continue;
            }
            dx = xj-xi; dy = yj-yi; dz = zj-zi;
            if (dx*dx + dy*dy + dz*dz < cut2) {
                nb_add_pair(adj,ia,ja,dx,dy);
            }
        }
    }
}
                             
/**
    Iterates through the cells and records all contacts in the
    provided nb list
 */
static void nb_fill_list(freesasa_nb *adj,
                         cell_list *c,
                         const freesasa_coord *coord,
                         const double *radii)
{
    int nc = c->n;
    for (int ic = 0; ic < nc; ++ic) {
        const cell *ci = &c->cell[ic];
        for (int jc = 0; jc < ci->n_nb; ++jc) {
            const cell *cj = ci->nb[jc];
            nb_calc_cell_pair(adj,coord,radii,ci,cj);
        }
    }
}

freesasa_nb *freesasa_nb_new(const freesasa_coord* coord,
                             const double *radii)
{
    if (coord == NULL || radii == NULL) return NULL;
    int n = freesasa_coord_n(coord);
    freesasa_nb *adj = freesasa_nb_alloc(n);
    if (!adj) return NULL;
    double cell_size = 2*max_array(radii,n);
    assert(cell_size > 0);
    cell_list *c = cell_list_new(cell_size,coord);
    assert(c);
    
    nb_fill_list(adj,c,coord,radii);
    cell_list_free(c);
    
    return adj;
}

int freesasa_nb_contact(const freesasa_nb *adj,
                        int i, int j)
{
    assert(adj != NULL);
    assert(i < adj->n && i >= 0);
    assert(j < adj->n && j >= 0);
    for (int k = 0; k < adj->nn[i]; ++k) {
        if (adj->nb[i][k] == j) return 1;
    }
    return 0;
}

