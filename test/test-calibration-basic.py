#!/usr/bin/python3

r'''Basic camera-calibration test

I observe, with noise, a number of chessboards from various angles with several
cameras. And I make sure that I can more or less compute the camera intrinsics
and extrinsics

'''

import sys
import numpy as np
import numpysane as nps
import os

testdir = os.path.dirname(os.path.realpath(__file__))

# I import the LOCAL mrcal since that's what I'm testing
sys.path[:0] = f"{testdir}/..",
import mrcal
import copy
import testutils

# I want the RNG to be deterministic
np.random.seed(0)

############# Set up my world, and compute all the perfect positions, pixel
############# observations of everything
models_ref = ( mrcal.cameramodel(f"{testdir}/data/cam0.opencv8.cameramodel"),
               mrcal.cameramodel(f"{testdir}/data/cam0.opencv8.cameramodel"),
               mrcal.cameramodel(f"{testdir}/data/cam1.opencv8.cameramodel"),
               mrcal.cameramodel(f"{testdir}/data/cam1.opencv8.cameramodel") )

imagersizes = nps.cat( *[m.imagersize() for m in models_ref] )
lensmodel   = models_ref[0].intrinsics()[0]
# I have opencv8 models_ref, but let me truncate to opencv4 models_ref to keep this
# simple and fast
lensmodel = 'LENSMODEL_OPENCV4'
for m in models_ref:
    m.intrinsics( intrinsics = (lensmodel, m.intrinsics()[1][:8]))
Nintrinsics = mrcal.getNlensParams(lensmodel)

Ncameras = len(models_ref)
Nframes  = 50

models_ref[0].extrinsics_rt_fromref(np.zeros((6,), dtype=float))
models_ref[1].extrinsics_rt_fromref(np.array((0.08,0.2,0.02, 1., 0.9,0.1)))
models_ref[2].extrinsics_rt_fromref(np.array((0.01,0.07,0.2, 2.1,0.4,0.2)))
models_ref[3].extrinsics_rt_fromref(np.array((-0.1,0.08,0.08, 4.4,0.2,0.1)))


pixel_uncertainty_stdev = 1.5
object_spacing          = 0.1
object_width_n          = 10
object_height_n         = 9
calobject_warp_ref      = np.array((0.002, -0.005))

# shapes (Nframes, Ncameras, Nh, Nw, 2),
#        (Nframes, 4,3)
q_ref,Rt_cam0_board_ref = \
    mrcal.make_synthetic_board_observations(models_ref,
                                            object_width_n, object_height_n, object_spacing,
                                            calobject_warp_ref,
                                            np.array((-2,   0,  4.0,  0.,  0.,  0.)),
                                            np.array((2.5, 2.5, 2.0, 40., 30., 30.)),
                                            Nframes)
frames_ref = mrcal.rt_from_Rt(Rt_cam0_board_ref)

############# I have perfect observations in q_ref. I corrupt them by noise
# weight has shape (Nframes, Ncameras, Nh, Nw),
weight01 = (np.random.rand(*q_ref.shape[:-1]) + 1.) / 2. # in [0,1]
weight0 = 0.2
weight1 = 1.0
weight = weight0 + (weight1-weight0)*weight01

# I want observations of shape (Nframes*Ncameras, Nh, Nw, 3) where each row is
# (x,y,weight)
observations_ref = nps.clump( nps.glue(q_ref,
                                       nps.dummy(weight,-1),
                                       axis=-1),
                              n=2)

def sample_dqref(observations):
    weight  = observations[...,-1]
    q_noise = np.random.randn(*observations.shape[:-1], 2) * pixel_uncertainty_stdev / nps.mv(nps.cat(weight,weight),0,-1)
    observations_perturbed = observations.copy()
    observations_perturbed[...,:2] += q_noise
    return q_noise, observations_perturbed

q_noise,observations = sample_dqref(observations_ref)


############# Now I pretend that the noisy observations are all I got, and I run
############# a calibration from those

# Dense observations. All the cameras see all the boards
indices_frame_camera = np.zeros( (Nframes*Ncameras, 2), dtype=np.int32)
indices_frame = indices_frame_camera[:,0].reshape(Nframes,Ncameras)
indices_frame.setfield(nps.outer(np.arange(Nframes, dtype=np.int32),
                                 np.ones((Ncameras,), dtype=np.int32)),
                       dtype = np.int32)
indices_camera = indices_frame_camera[:,1].reshape(Nframes,Ncameras)
indices_camera.setfield(nps.outer(np.ones((Nframes,), dtype=np.int32),
                                 np.arange(Ncameras, dtype=np.int32)),
                       dtype = np.int32)

indices_frame_camintrinsics_camextrinsics = \
    nps.glue(indices_frame_camera,
             indices_frame_camera[:,(1,)]-1,
             axis=-1)

def optimize( intrinsics,
              extrinsics_rt_fromref,
              frames_rt_toref,
              observations,
              indices_frame_camintrinsics_camextrinsics,

              calobject_warp                    = None,
              do_optimize_intrinsic_core        = False,
              do_optimize_intrinsic_distortions = False,
              do_optimize_extrinsics            = False,
              do_optimize_frames                = False,
              do_optimize_calobject_warp        = False,
              get_covariances                   = False):
    r'''Run the optimizer

    Function arguments are read-only. The optimization results, in various
    forms, are returned.

    Some global variables are used to provide input: these never change
    throughout this whole program:

    - lensmodel
    - imagersizes
    - object_spacing, object_width_n, object_height_n
    - pixel_uncertainty_stdev

    '''

    intrinsics            = copy.deepcopy(intrinsics)
    extrinsics_rt_fromref = copy.deepcopy(extrinsics_rt_fromref)
    frames_rt_toref       = copy.deepcopy(frames_rt_toref)
    calobject_warp        = copy.deepcopy(calobject_warp)

    solver_context = mrcal.SolverContext()
    stats = mrcal.optimize( intrinsics, extrinsics_rt_fromref, frames_rt_toref, None,
                            observations, indices_frame_camintrinsics_camextrinsics,
                            None, None, lensmodel,
                            calobject_warp              = calobject_warp,
                            imagersizes                 = imagersizes,
                            calibration_object_spacing  = object_spacing,
                            calibration_object_width_n  = object_width_n,
                            calibration_object_height_n = object_height_n,
                            skip_regularization         = False,
                            verbose                     = False,

                            observed_pixel_uncertainty  = pixel_uncertainty_stdev,
                            skip_outlier_rejection      = True,

                            do_optimize_frames                = do_optimize_frames,
                            do_optimize_intrinsic_core        = do_optimize_intrinsic_core,
                            do_optimize_intrinsic_distortions = do_optimize_intrinsic_distortions,
                            do_optimize_extrinsics            = do_optimize_extrinsics,
                            do_optimize_calobject_warp        = do_optimize_calobject_warp,
                            get_covariances                   = get_covariances,
                            solver_context                    = solver_context)

    covariance_intrinsics = stats.get('covariance_intrinsics')
    covariance_extrinsics = stats.get('covariance_extrinsics')
    p_packed = solver_context.p().copy()

    return \
        intrinsics, extrinsics_rt_fromref, frames_rt_toref, calobject_warp,   \
        p_packed, stats['x'], stats['rms_reproj_error__pixels'], \
        covariance_intrinsics, covariance_extrinsics,     \
        solver_context





intrinsics_data,extrinsics_rt_fromref,frames_rt_toref = \
    mrcal.make_seed_no_distortion(imagersizes          = imagersizes,
                                  focal_estimate       = 1500,
                                  Ncameras             = Ncameras,
                                  indices_frame_camera = indices_frame_camera,
                                  observations         = observations,
                                  object_spacing       = object_spacing,
                                  object_width_n       = object_width_n,
                                  object_height_n      = object_height_n)

# I have a pinhole intrinsics estimate. Mount it into a full distortiony model,
# seeded with random numbers
intrinsics = np.zeros((Ncameras,Nintrinsics), dtype=float)
intrinsics[:,:4] = intrinsics_data
intrinsics[:,4:] = np.random.random( (Ncameras, intrinsics.shape[1]-4) ) * 1e-6

# Simpler pre-solves
intrinsics, extrinsics_rt_fromref, frames_rt_toref, calobject_warp, \
p_packed, x, rmserr,                            \
covariance_intrinsics, covariance_extrinsics,   \
solver_context =                                \
    optimize(intrinsics, extrinsics_rt_fromref, frames_rt_toref, observations,
             indices_frame_camintrinsics_camextrinsics,
             do_optimize_extrinsics            = True,
             do_optimize_frames                = True)
intrinsics, extrinsics_rt_fromref, frames_rt_toref, calobject_warp, \
p_packed, x, rmserr,                            \
covariance_intrinsics, covariance_extrinsics,   \
solver_context =                                \
    optimize(intrinsics, extrinsics_rt_fromref, frames_rt_toref, observations,
             indices_frame_camintrinsics_camextrinsics,
             do_optimize_intrinsic_core        = True,
             do_optimize_extrinsics            = True,
             do_optimize_frames                = True)

# Complete final solve
calobject_warp = np.array((0.001, 0.001))
intrinsics, extrinsics_rt_fromref, frames_rt_toref, calobject_warp, \
p_packed, x, rmserr,                            \
covariance_intrinsics, covariance_extrinsics,   \
solver_context =                                \
    optimize(intrinsics, extrinsics_rt_fromref, frames_rt_toref, observations,
             indices_frame_camintrinsics_camextrinsics,
             calobject_warp                    = calobject_warp,
             do_optimize_intrinsic_core        = True,
             do_optimize_intrinsic_distortions = True,
             do_optimize_extrinsics            = True,
             do_optimize_frames                = True,
             do_optimize_calobject_warp        = True,
             get_covariances                   = True)


############# Calibration computed. Now I see how well I did

models_solved = \
    [ mrcal.cameramodel( imagersize            = imagersizes[i],
                         intrinsics            = (lensmodel, intrinsics[i,:])) \
      for i in range(Ncameras)]
for i in range(1,Ncameras):
    models_solved[i].extrinsics_rt_fromref( extrinsics_rt_fromref[i-1,:] )

testutils.confirm_equal(rmserr, 0,
                        eps = 2.5,
                        msg = "Converged to a low RMS error")

testutils.confirm_equal( calobject_warp,
                         calobject_warp_ref,
                         eps = 2e-3,
                         msg = "Recovered the calibration object shape" )

testutils.confirm_equal( np.std(x),
                         pixel_uncertainty_stdev,
                         eps = pixel_uncertainty_stdev*0.1,
                         msg = "Residual have the expected distribution" )

# Checking the extrinsics. These aren't defined absolutely: each solve is free
# to put the observed frames anywhere it likes. The intrinsics-diff code
# computes a compensating rotation to address this. Here I simply look at the
# relative transformations between cameras, which would cancel out any extra
# rotations. AND since camera0 is fixed at the identity transformation, I can
# simply look at each extrinsics transformation.
for icam in range(1,len(models_ref)):

    Rt_extrinsics_err = \
        mrcal.compose_Rt( models_solved[icam].extrinsics_Rt_fromref(),
                          models_ref   [icam].extrinsics_Rt_toref() )

    testutils.confirm_equal( nps.mag(Rt_extrinsics_err[3,:]),
                             0.0,
                             eps = 0.05,
                             msg = f"Recovered extrinsic translation for camera {icam}")

    testutils.confirm_equal( (np.trace(Rt_extrinsics_err[:3,:]) - 1) / 2.,
                             1.0,
                             eps = np.cos(1. * np.pi/180.0), # 1 deg
                             msg = f"Recovered extrinsic rotation for camera {icam}")

Rt_frame_err = \
    mrcal.compose_Rt( mrcal.Rt_from_rt(frames_rt_toref),
                      mrcal.invert_Rt(Rt_cam0_board_ref) )

testutils.confirm_equal( np.max(nps.mag(Rt_frame_err[..., 3,:])),
                         0.0,
                         eps = 0.08,
                         msg = "Recovered frame translation")
testutils.confirm_equal( np.min( (nps.trace(Rt_frame_err[..., :3,:]) - 1)/2. ),
                         1.0,
                         eps = np.cos(1. * np.pi/180.0), # 1 deg
                         msg = "Recovered frame rotation")


# Checking the intrinsics. Each intrinsics vector encodes an implicit rotation.
# I compute and compensate for this rotation when making my intrinsics
# comparisons. I make sure that within some distance of the pixel center, the
# projections match up to within some number of pixels
Nw = 60
def projection_diff(models_ref, max_dist_from_center, fit_Rcompensating = True):
    lensmodels      = [model.intrinsics()[0] for model in models_ref]
    intrinsics_data = [model.intrinsics()[1] for model in models_ref]

    # shape (..., Nh,Nw, ...)
    v,q0 = \
        mrcal.sample_imager_unproject(Nw,None,
                                      *imagersizes[0],
                                      lensmodels, intrinsics_data)
    Rcompensating01 = \
        mrcal.compute_Rcompensating(q0,
                                    v[0,...], v[1,...],
                                    focus_center = None,
                                    focus_radius = -1 if fit_Rcompensating else 0,
                                    imagersizes  = imagersizes)
    q1 = mrcal.project(nps.matmult(v[0,...],Rcompensating01),
                       lensmodels[1], intrinsics_data[1])
    diff = nps.mag(q1 - q0)

    # zero-out everything too far from the center
    center = (imagersizes[0] - 1.) / 2.
    diff[ nps.norm2(q0 - center) > max_dist_from_center*max_dist_from_center ] = 0
    # gp.plot(diff,
    #         ascii = True,
    #         using = mrcal.imagergrid_using(imagersizes[0], Nw),
    #         square=1, _with='image', tuplesize=3, hardcopy='/tmp/yes.gp', cbmax=3)

    return diff


for icam in range(len(models_ref)):
    diff = projection_diff( (models_ref[icam], models_solved[icam]), 800, True)

    testutils.confirm_equal(diff, 0,
                            worstcase = True,
                            eps = 4.,
                            msg = f"Recovered intrinsics for camera {icam}")


############# Basic checks all done. Now I look at uncertainties

# The uncertainty computation is documented in the docstring for
# compute_projection_stdev(). The math and the implementation are tricky,
# so I empirically confirm that the thing being computed is correct, both in
# implementation and intent.
#
# I use dense linear algebra to compute the reference arrays. This is
# inefficient, but easy to write, and is useful for checking the more complex
# sparse implementations of the main library
def callback_tweaked_intrinsics(intrinsics_data):
    x,Joptimizer = \
        mrcal.optimizerCallback(intrinsics_data,extrinsics_rt_fromref,frames_rt_toref, None,
                                observations, indices_frame_camintrinsics_camextrinsics,
                                None, None,
                                lensmodel,
                                do_optimize_calobject_warp        = True,
                                calobject_warp                    = calobject_warp,
                                do_optimize_intrinsic_core        = True,
                                do_optimize_intrinsic_distortions = True,
                                do_optimize_extrinsics            = True,

                                imagersizes                       = imagersizes,
                                calibration_object_spacing        = object_spacing,
                                calibration_object_width_n        = object_width_n,
                                calibration_object_height_n       = object_height_n,
                                skip_regularization               = False,
                                verbose                           = False )
    Joptimizer = Joptimizer.toarray()
    J = Joptimizer.copy()
    solver_context.pack(J)
    return x,J,Joptimizer


# State and measurements at the optimal operating point
p_packed0 = p_packed.copy()
x0,J0,J_packed0 = callback_tweaked_intrinsics(intrinsics)

###########################################################################
# First a very basic gradient check. Looking at an arbitrary camera's
# intrinsics. The test-gradients tool does this much more thoroughly
icam        = 1
delta       = np.random.randn(Nintrinsics) * 1e-6
ivar        = solver_context.state_index_intrinsics(icam)
J0_slice    = J0[:,ivar:ivar+Nintrinsics]
intrinsics_perturbed = intrinsics.copy()
intrinsics_perturbed[icam] += delta
x1                   = callback_tweaked_intrinsics(intrinsics_perturbed)[0]
dx                   = x1 - x0
dx_predicted         = nps.inner(J0_slice, delta)
testutils.confirm_equal( dx_predicted, dx,
                         eps = 1e-3,
                         worstcase = True,
                         relative = True,
                         msg = "Trivial, sanity-checking gradient check")

###########################################################################
# We're supposed to be at the optimum. E = norm2(x) ~ norm2(x0 + J dp) =
# norm2(x0) + 2 dpt Jt x0 + norm2(J dp). At the optimum Jt x0 = 0 -> E =
# norm2(x0) + norm2(J dp). dE = norm2(J dp) = norm2(dx_predicted)
x_predicted  = x0 + dx_predicted
dE           = nps.norm2(x1) - nps.norm2(x0)
dE_predicted = nps.norm2(dx_predicted)
testutils.confirm_equal( dE_predicted, dE,
                         eps = 1e-3,
                         relative = True,
                         msg = "diff(E) predicted")

###########################################################################
# Fine. Let's make sure the noise propagation works as it should. First off, is
# the implementation correct? Derivation in compute_projection_stdev()
# says intrinsics covariance:
#
#   Var(intrinsics) = (inv(JtJ)[intrinsicsrows] Jobservationst)
#                     (inv(JtJ)[intrinsicsrows] Jobservationst)t
#                     s^2
#
# Let's make sure the computation was done correctly
Nobservations_board = indices_frame_camera.shape[0]
Nmeasurements_board = Nobservations_board * object_height_n * object_width_n * 2

invJtJ = np.linalg.inv(nps.matmult(nps.transpose(J0), J0))
J0observations = J0[:Nmeasurements_board,:]

for icam in range(Ncameras):

    ivar0 = Nintrinsics*icam
    ivar1 = Nintrinsics*icam + Nintrinsics
    covariance_intrinsics_predicted = \
        pixel_uncertainty_stdev*pixel_uncertainty_stdev * \
        nps.matmult( invJtJ[ivar0:ivar1,:],
                     nps.transpose(J0observations),
                     J0observations,
                     invJtJ[:,ivar0:ivar1])

    testutils.confirm_equal( covariance_intrinsics_predicted,
                             covariance_intrinsics[icam, ...],
                             relative = True,
                             eps = 1e-3,
                             msg = f"covariance_intrinsics computed correctly for camera {icam}")

###########################################################################
# Same thing for covariance_extrinsics. Are we computing what we think we're
# computing? First, let's look at all the extrinsics together as one big vector
ivar0 = Nintrinsics*Ncameras
ivar1 = Nintrinsics*Ncameras + 6*(Ncameras-1)

covariance_extrinsics_predicted = \
    pixel_uncertainty_stdev*pixel_uncertainty_stdev * \
    nps.matmult( invJtJ[ ivar0:ivar1, :],
                 nps.transpose(J0observations),
                 J0observations,
                 invJtJ[:, ivar0:ivar1 ])
testutils.confirm_equal( covariance_extrinsics_predicted,
                         covariance_extrinsics,
                         relative = True,
                         eps = 1e-3,
                         msg = f"covariance_extrinsics computed correctly")


###########################################################################
# I confirmed that I'm computing what I think I'm computing. Do the desired
# expressions do what they're supposed to do? I perturb my input observation
# vector qref by dqref. The effect on the parameters should be dp = M dqref.
# Where M = inv(JtJ) Jobservationst W
dqref, observations_perturbed = sample_dqref(observations)
_,_,_,_,         \
p_packed1, _, _, \
_,_,             \
solver_context = \
    optimize(intrinsics, extrinsics_rt_fromref, frames_rt_toref, observations_perturbed,
             indices_frame_camintrinsics_camextrinsics,
             calobject_warp                    = calobject_warp,
             do_optimize_intrinsic_core        = True,
             do_optimize_intrinsic_distortions = True,
             do_optimize_extrinsics            = True,
             do_optimize_frames                = True,
             do_optimize_calobject_warp        = True)

dp = p_packed1-p_packed0
w = observations[..., np.array((2,2))].ravel()
M = np.linalg.solve( nps.matmult(nps.transpose(J_packed0),J_packed0),
                     nps.transpose(J_packed0[:Nmeasurements_board, :]) ) * w
dp_predicted = nps.matmult( dqref.ravel(), nps.transpose(M)).ravel()

slice_intrinsics = slice(0,                                       solver_context.state_index_camera_rt(0))
slice_extrinsics = slice(solver_context.state_index_camera_rt(0), solver_context.state_index_frame_rt(0))
slice_frames     = slice(solver_context.state_index_frame_rt(0),  None)

# These thresholds look terrible. And they are. But I'm pretty sure this is
# working properly. Look at the plots
testutils.confirm_equal( dp_predicted[slice_intrinsics],
                         dp          [slice_intrinsics],
                         relative  = True,
                         eps = 0.4,
                         msg = f"Predicted dp from dqref: intrinsics")
testutils.confirm_equal( dp_predicted[slice_extrinsics],
                         dp          [slice_extrinsics],
                         relative  = True,
                         eps = 0.4,
                         msg = f"Predicted dp from dqref: extrinsics")
testutils.confirm_equal( dp_predicted[slice_frames],
                         dp          [slice_frames],
                         relative  = True,
                         eps = 0.5,
                         msg = f"Predicted dp from dqref: frames")

# To see the expected and observed shift in the optimal parameters
#
# import gnuplotlib as gp
# plot_dp = gp.gnuplotlib(title='Parameter shift due to input observations shift',
#                         xlabel='Parameter',
#                         ylabel='Deviation')
# plot_dp.plot(nps.cat(dp, dp_predicted), legend=np.array(('dp_reoptimized', 'dp_reoptimized_predicted')))

testutils.finish()