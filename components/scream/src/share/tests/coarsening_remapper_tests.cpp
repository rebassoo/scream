#include <catch2/catch.hpp>

#include "share/grid/remap/coarsening_remapper.hpp"
#include "share/grid/point_grid.hpp"
#include "share/io/scream_scorpio_interface.hpp"

namespace scream {

template<typename ViewT>
typename ViewT::HostMirror
cmvc (const ViewT& v) {
  auto vh = Kokkos::create_mirror_view(v);
  Kokkos::deep_copy(vh,v);
  return vh;
}

class CoarseningRemapperTester : public CoarseningRemapper {
public:
  CoarseningRemapperTester (const grid_ptr_type& src_grid,
                            const std::string& map_file)
   : CoarseningRemapper(src_grid,map_file)
  {
    // Nothing to do
  }
  view_1d<gid_t>::HostMirror
  test_triplet_gids (const std::string& map_file) const {
    return CoarseningRemapper::get_my_triplets_gids (map_file,m_src_grid);
  }

  view_1d<int> get_row_offsets () const {
    return m_row_offsets;
  }
  view_1d<int> get_col_lids () const {
    return m_col_lids;
  }
  view_1d<Real> get_weights () const {
    return m_weights;
  }

  grid_ptr_type get_ov_tgt_grid () const {
    return m_ov_tgt_grid;
  }

  view_2d<int>::HostMirror get_send_f_pid_offsets () const {
    return cmvc(m_send_f_pid_offsets);
  }
  view_2d<int>::HostMirror get_recv_f_pid_offsets () const {
    return cmvc(m_recv_f_pid_offsets);
  }

  view_1d<int>::HostMirror get_recv_lids_beg () const {
    return cmvc(m_recv_lids_beg);
  }
  view_1d<int>::HostMirror get_recv_lids_end () const {
    return cmvc(m_recv_lids_end);
  }

  view_2d<int>::HostMirror get_send_lids_pids () const {
    return cmvc(m_send_lids_pids );
  }
  view_2d<int>::HostMirror get_recv_lids_pidpos () const {
    return cmvc(m_recv_lids_pidpos);
  }

  view_1d<int>::HostMirror get_send_pid_lids_start () const {
    return cmvc(m_send_pid_lids_start);
  }

  int gid2lid (const gid_t gid, const grid_ptr_type& grid) const {
    return CoarseningRemapper::gid2lid(gid,grid);
  }
};

template<typename ViewT>
bool contains (const ViewT& v, const typename ViewT::traits::value_type& entry) {
  const auto vh = cmvc (v);
  const auto beg = vh.data();
  const auto end = vh.data() + vh.size();
  for (auto it=beg; it!=end; ++it) {
    if (*it == entry) {
      return true;
    }
  }
  return false;
}

void print (const std::string& msg, const ekat::Comm& comm) {
  if (comm.am_i_root()) {
    printf("%s",msg.c_str());
  }
}

TEST_CASE ("coarsening_remap") {

  // -------------------------------------- //
  //           Init MPI and PIO             //
  // -------------------------------------- //

  ekat::Comm comm(MPI_COMM_WORLD);

  MPI_Fint fcomm = MPI_Comm_c2f(comm.mpi_comm());
  scorpio::eam_init_pio_subsystem(fcomm);

  // -------------------------------------- //
  //           Set grid/map sizes           //
  // -------------------------------------- //

  const int nldofs_src = 10;
  const int nldofs_tgt =  5;
  const int ngdofs_src = nldofs_src*comm.size();
  const int ngdofs_tgt = nldofs_tgt*comm.size();
  const int nnz = ngdofs_src;

  // -------------------------------------- //
  //           Create a map file            //
  // -------------------------------------- //

  print (" -> creating map file ...\n",comm);

  std::string filename = "coarsening_map_file_np" + std::to_string(comm.size()) + ".nc";
  scorpio::register_file(filename, scorpio::FileMode::Write);

  scorpio::register_dimension(filename,"n_a", "n_a", ngdofs_src);
  scorpio::register_dimension(filename,"n_b", "n_b", ngdofs_tgt);
  scorpio::register_dimension(filename,"n_s", "n_s", nnz);

  scorpio::register_variable(filename,"col","col","none",{"n_s"},"real","int","Real-nnz");
  scorpio::register_variable(filename,"row","row","none",{"n_s"},"real","int","Real-nnz");
  scorpio::register_variable(filename,"S","S","none",{"n_s"},"real","real","Real-nnz");

  std::vector<std::int64_t> dofs (nldofs_src);
  std::iota(dofs.begin(),dofs.end(),comm.rank()*nldofs_src);
  scorpio::set_dof(filename,"col",nldofs_src,dofs.data());
  scorpio::set_dof(filename,"row",nldofs_src,dofs.data());
  scorpio::set_dof(filename,"S",nldofs_src,dofs.data());
  
  scorpio::eam_pio_enddef(filename);

  // Create triplets: tgt entry K is the avg of src entries K and K+ngdofs_tgt
  // NOTE: add 1 to row/col indices, since e3sm map files indices are 1-based
  std::vector<Real> col,row,S;
  for (int i=0; i<nldofs_tgt; ++i) {
    row.push_back(1+i+nldofs_tgt*comm.rank());
    col.push_back(1+i+nldofs_tgt*comm.rank());
    S.push_back(0.25);

    row.push_back(1+i+nldofs_tgt*comm.rank());
    col.push_back(1+i+nldofs_tgt*comm.rank() + ngdofs_tgt);
    S.push_back(0.75);
  }

  scorpio::grid_write_data_array(filename,"row",row.data(),row.size());
  scorpio::grid_write_data_array(filename,"col",col.data(),row.size());
  scorpio::grid_write_data_array(filename,"S",S.data(),row.size());

  scorpio::eam_pio_closefile(filename);
  print (" -> creating map file ... done!\n",comm);

  // -------------------------------------- //
  //      Build src grid and remapper       //
  // -------------------------------------- //

  print (" -> creating grid and remapper ...\n",comm);

  AbstractGrid::dofs_list_type src_dofs("",nldofs_src);
  auto src_dofs_h = cmvc(src_dofs);
  std::iota(src_dofs_h.data(),src_dofs_h.data()+nldofs_src,nldofs_src*comm.rank());
  Kokkos::deep_copy(src_dofs,src_dofs_h);

  auto src_grid = std::make_shared<PointGrid>("src",nldofs_src,20,comm);
  src_grid->set_dofs(src_dofs);

  auto remap = std::make_shared<CoarseningRemapperTester>(src_grid,filename);
  print (" -> creating grid and remapper ... done!\n",comm);

  // -------------------------------------- //
  //      Create src/tgt grid fields        //
  // -------------------------------------- //

  print (" -> creating fields ...\n",comm);
  constexpr int vec_dim = 3;
  auto create_field = [&] (const std::string& name,
                           const std::shared_ptr<const AbstractGrid>& grid,
                           const bool twod, const bool vec, const bool mid = false, const int ps = 1) -> Field
  {
    constexpr auto CMP = FieldTag::Component;
    constexpr auto units = ekat::units::Units::nondimensional();
    auto fl = twod
            ? (vec ? grid->get_2d_vector_layout (CMP,vec_dim)
                   : grid->get_2d_scalar_layout ())
            : (vec ? grid->get_3d_vector_layout (mid,CMP,vec_dim)
                   : grid->get_3d_scalar_layout (mid));
    FieldIdentifier fid(name,fl,units,grid->name());
    Field f(fid);
    f.get_header().get_alloc_properties().request_allocation(ps);
    f.allocate_view();
    return f;
  };

  auto tgt_grid = remap->get_tgt_grid();

  auto src_s2d   = create_field("s2d",  src_grid,true,false);
  auto src_v2d   = create_field("v2d",  src_grid,true,true);
  auto src_s3d_m = create_field("s3d_m",src_grid,false,false,true, 1);
  auto src_s3d_i = create_field("s3d_i",src_grid,false,false,false,std::min(SCREAM_PACK_SIZE,4));
  auto src_v3d_m = create_field("v3d_m",src_grid,false,true ,true, std::min(SCREAM_PACK_SIZE,8));
  auto src_v3d_i = create_field("v3d_i",src_grid,false,true ,false,std::min(SCREAM_PACK_SIZE,16));

  auto tgt_s2d   = create_field("s2d",  tgt_grid,true,false);
  auto tgt_v2d   = create_field("v2d",  tgt_grid,true,true);
  auto tgt_s3d_m = create_field("s3d_m",tgt_grid,false,false,true, 1);
  auto tgt_s3d_i = create_field("s3d_i",tgt_grid,false,false,false,std::min(SCREAM_PACK_SIZE,4));
  auto tgt_v3d_m = create_field("v3d_m",tgt_grid,false,true ,true, std::min(SCREAM_PACK_SIZE,8));
  auto tgt_v3d_i = create_field("v3d_i",tgt_grid,false,true ,false,std::min(SCREAM_PACK_SIZE,16));

  std::vector<Field> src_f = {src_s2d,src_v2d,src_s3d_m,src_s3d_i,src_v3d_m,src_v3d_i};
  std::vector<Field> tgt_f = {tgt_s2d,tgt_v2d,tgt_s3d_m,tgt_s3d_i,tgt_v3d_m,tgt_v3d_i};

  const int nfields = src_f.size();

  std::vector<int> field_col_size (src_f.size());
  std::vector<int> field_col_offset (src_f.size()+1,0);
  int sum_fields_col_sizes = 0;
  for (int i=0; i<nfields; ++i) {
    const auto& f  = src_f[i];  // Doesn't matter if src or tgt
    const auto& fl = f.get_header().get_identifier().get_layout();
    field_col_size[i] = fl.size() / fl.dim(0);
    sum_fields_col_sizes += field_col_size[i];
    field_col_offset[i+1] = field_col_offset[i]+field_col_size[i];
  }

  print (" -> creating fields ... done!\n",comm);

  // -------------------------------------- //
  //     Register fields in the remapper    //
  // -------------------------------------- //

  print (" -> registering fields ...\n",comm);
  remap->registration_begins();
  remap->register_field(src_s2d,  tgt_s2d);
  remap->register_field(src_v2d,  tgt_v2d);
  remap->register_field(src_s3d_m,tgt_s3d_m);
  remap->register_field(src_s3d_i,tgt_s3d_i);
  remap->register_field(src_v3d_m,tgt_v3d_m);
  remap->register_field(src_v3d_i,tgt_v3d_i);
  remap->registration_ends();
  print (" -> registering fields ... done!\n",comm);

  // -------------------------------------- //
  //        Check remapper internals        //
  // -------------------------------------- //

  print (" -> Checking remapper internal state ...\n",comm);

  // Check tgt grid
  REQUIRE (tgt_grid->get_num_global_dofs()==ngdofs_tgt);

  // Check which triplets are read from map file
  auto my_triplets = remap->test_triplet_gids (filename);
  const int num_triplets = my_triplets.size();
  REQUIRE (num_triplets==nldofs_src);
  for (int i=0; i<nldofs_src; ++i) {
    const auto src_gid = src_dofs_h(i);
    const auto tgt_gid = src_gid % ngdofs_tgt;

    REQUIRE (contains(my_triplets, 2*tgt_gid + src_gid/ngdofs_tgt));
  }

  // Check overlapped tgt grid
  // NOTE: you need to treat the case of 1 rank separately, since in that case
  //       there are 2 local src dofs impacting the same tgt dof, while with 2+
  //       ranks every local src dof impacts a different tgt dof.
  auto ov_tgt_grid = remap->get_ov_tgt_grid ();
  const int num_loc_ov_tgt_gids = ov_tgt_grid->get_num_local_dofs();
  const int expected_num_loc_ov_tgt_gids = ngdofs_tgt>=nldofs_src ? nldofs_src : ngdofs_tgt;
  REQUIRE (num_loc_ov_tgt_gids==expected_num_loc_ov_tgt_gids);
  auto ov_gids = ov_tgt_grid->get_dofs_gids_host();
  for (int i=0; i<num_loc_ov_tgt_gids; ++i) {
    if (comm.size()==1) {
      REQUIRE(ov_gids[i]==i);
    } else {
      const auto src_gid = src_dofs_h[i];
      REQUIRE (contains(ov_gids, src_gid % ngdofs_tgt));
    }
  }

  // Check sparse matrix
  auto row_offsets_h = cmvc(remap->get_row_offsets());
  auto col_lids_h    = cmvc(remap->get_col_lids());
  auto weights_h = cmvc(remap->get_weights());
  auto ov_tgt_gids = ov_tgt_grid->get_dofs_gids_host();
  auto src_gids    = remap->get_src_grid()->get_dofs_gids_host();

  REQUIRE (col_lids_h.extent_int(0)==nldofs_src);
  REQUIRE (row_offsets_h.extent_int(0)==(num_loc_ov_tgt_gids+1));
  for (int i=0; i<num_loc_ov_tgt_gids; ++i) {
    if (comm.size()==1) {
      REQUIRE (row_offsets_h(i)==(2*i));
    } else {
      REQUIRE (row_offsets_h(i)==i);
    }
  }
  REQUIRE (row_offsets_h(num_loc_ov_tgt_gids)==nldofs_src);

  for (int irow=0; irow<num_loc_ov_tgt_gids; ++irow) {
    const auto row_gid = ov_tgt_gids(irow);
    for (int innz=row_offsets_h(irow); innz<row_offsets_h(irow+1); ++innz) {
      const auto col_lid = col_lids_h(innz);
      const auto col_gid = src_gids(col_lid);
      if (row_gid==col_gid) {
        REQUIRE (weights_h(innz)==0.25);
      } else {
        REQUIRE (weights_h(innz)==0.75);
      }
    }
  }

  // Check internal MPI structures
  const int num_loc_tgt_gids = tgt_grid->get_num_local_dofs();
  const auto tgt_gids = tgt_grid->get_dofs_gids_host();
  const auto recv_lids_beg = remap->get_recv_lids_beg();
  const auto recv_lids_end = remap->get_recv_lids_end();
  const auto recv_lids_pidpos = remap->get_recv_lids_pidpos();
  // Rank 0 sends everything to itself
  for (int i=0; i<num_loc_tgt_gids; ++i) {
    if (comm.size()==1) {
      // Each tgt dof has one ov_tgt contribution,
      // since the matvec is fully local
      REQUIRE (recv_lids_beg(i)==i);
      REQUIRE (recv_lids_end(i)==(i+1));

      REQUIRE (recv_lids_pidpos(i,0)==comm.rank());
      REQUIRE (recv_lids_pidpos(i,1)==i);
    } else {
      // Each tgt dof has two ov_tgt contributions,
      // since the mat vec happens on two different PIDs.
      REQUIRE (recv_lids_beg(i)==2*i);
      REQUIRE (recv_lids_end(i)==(2*i+2));
      // Figure out where the contributions come from
      const auto src1 = tgt_gids(i);
      const auto src2 = src1 + ngdofs_tgt;
      const auto pid1 = src1 / nldofs_src;
      const auto pid2 = src2 / nldofs_src;
      REQUIRE (recv_lids_pidpos(2*i,0)==pid1);
      REQUIRE (recv_lids_pidpos(2*i+1,0)==pid2);
    }
  }
  print (" -> Checking remapper internal state ... OK!\n",comm);

  // -------------------------------------- //
  //       Generate data for src fields     //
  // -------------------------------------- //

  print (" -> generate src fields data ...\n",comm);
  // Generate data in a deterministic way, so that when we check results,
  // we know a priori what the input data that generated the tgt field's
  // values was, even if that data was off rank.
  for (const auto& f : src_f) {
    const auto& l = f.get_header().get_identifier().get_layout();
    switch (get_layout_type(l.tags())) {
      case LayoutType::Scalar2D:
      {
        const auto v_src = f.get_view<Real*,Host>();
        for (int i=0; i<nldofs_src; ++i) {
          v_src(i) = src_gids(i);
        }
      } break;
      case LayoutType::Vector2D:
      {
        const auto v_src = f.get_view<Real**,Host>();
        for (int i=0; i<nldofs_src; ++i) {
          for (int j=0; j<vec_dim; ++j) {
            v_src(i,j) = src_gids(i)*vec_dim + j;
        }}
      } break;
      case LayoutType::Scalar3D:
      {
        const int nlevs = l.dims().back();
        const auto v_src = f.get_view<Real**,Host>();
        for (int i=0; i<nldofs_src; ++i) {
          for (int j=0; j<nlevs; ++j) {
            v_src(i,j) = src_gids(i)*nlevs + j;
        }}
      } break;
      case LayoutType::Vector3D:
      {
        const int nlevs = l.dims().back();
        const auto v_src = f.get_view<Real***,Host>();
        for (int i=0; i<nldofs_src; ++i) {
          for (int j=0; j<vec_dim; ++j) {
            for (int k=0; k<nlevs; ++k) {
              v_src(i,j,k) = src_gids(i)*vec_dim*nlevs + j*nlevs + k;
        }}}
      } break;
      default:
        EKAT_ERROR_MSG ("Unexpected layout.\n");
    }
    f.sync_to_dev();
  }
  print (" -> generate src fields data ... done!\n",comm);

  auto combine = [] (const Real lhs, const Real rhs) -> Real {
    return 0.25*lhs + 0.75*rhs;
  };

  // No bwd remap
  REQUIRE_THROWS(remap->remap(false));

  for (int irun=0; irun<5; ++irun) {
    print (" -> run remap ...\n",comm);
    remap->remap(true);
    print (" -> run remap ... done!\n",comm);

    // -------------------------------------- //
    //          Check remapped fields         //
    // -------------------------------------- //

    print (" -> check tgt fields ...\n",comm);
    // Recall, tgt gid K should be the avg of src gids K and K+ngdofs_tgt
    const int ntgt_gids = tgt_gids.size();
    for (size_t ifield=0; ifield<tgt_f.size(); ++ifield) {
      const auto& f = tgt_f[ifield];
      const auto& l = f.get_header().get_identifier().get_layout();
      const auto ls = to_string(l);
      std::string dots (25-ls.size(),'.');
      print ("   -> Checking field with layout " + to_string(l) + " " + dots + "\n",comm);

      f.sync_to_host();

      switch (get_layout_type(l.tags())) {
        case LayoutType::Scalar2D:
        {
          const auto v_tgt = f.get_view<const Real*,Host>();
          for (int i=0; i<ntgt_gids; ++i) {
            const auto gid = tgt_gids(i);
            const auto term1 = gid;
            const auto term2 = gid+ngdofs_tgt;
            REQUIRE ( v_tgt(i)== combine(term1,term2) );
          }
        } break;
        case LayoutType::Vector2D:
        {
          const auto v_tgt = f.get_view<const Real**,Host>();
          for (int i=0; i<ntgt_gids; ++i) {
            const auto gid = tgt_gids(i);
            for (int j=0; j<vec_dim; ++j) {
              const auto term1 = gid*vec_dim+j;
              const auto term2 = (gid+ngdofs_tgt)*vec_dim+j;
              REQUIRE ( v_tgt(i,j)== combine(term1,term2) );
          }}
        } break;
        case LayoutType::Scalar3D:
        {
          const int nlevs = l.dims().back();
          const auto v_tgt = f.get_view<const Real**,Host>();
          for (int i=0; i<ntgt_gids; ++i) {
            const auto gid = tgt_gids(i);
            for (int j=0; j<nlevs; ++j) {
              const auto term1 = gid*nlevs+j;
              const auto term2 = (gid+ngdofs_tgt)*nlevs+j;
              REQUIRE ( v_tgt(i,j)== combine(term1,term2) );
          }}
        } break;
        case LayoutType::Vector3D:
        {
          const int nlevs = l.dims().back();
          const auto v_tgt = f.get_view<const Real***,Host>();
          for (int i=0; i<ntgt_gids; ++i) {
            const auto gid = tgt_gids(i);
            for (int j=0; j<vec_dim; ++j) {
              for (int k=0; k<nlevs; ++k) {
                const auto term1 = gid*vec_dim*nlevs+j*nlevs+k;
                const auto term2 = (gid+ngdofs_tgt)*vec_dim*nlevs+j*nlevs+k;
                REQUIRE ( v_tgt(i,j,k)== combine(term1,term2) );
          }}}
        } break;
        default:
          EKAT_ERROR_MSG ("Unexpected layout.\n");
      }

      print ("   -> Checking field with layout " + to_string(l) + " " + dots + " OK!\n",comm);
    }
    print ("check tgt fields ... done!\n",comm);
  }

  // Clean up scorpio stuff
  scorpio::eam_pio_finalize();
}

} // namespace scream
