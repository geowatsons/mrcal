#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <dogleg.h>
#include <minimath.h>
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

// This is a workaround for OpenCV's stupidity: they decided to break their C
// API. Without this you get undefined references to cvRound() if you build
// without optimizations.
static inline int cvRound(float value)
{
    return (int)(value + (value >= 0 ? 0.5f : -0.5f));
}

#include <opencv2/calib3d/calib3d_c.h>

#include "mrcal.h"

#warning terminology: measurement and observation are the same?

#warning kill this comment?
/*

calibration and sfm formulations are a little different

- calibration

  cameras stationary, observed objects move

  ref coord system: cam0

  state:
    for cameras: poses of cameras (except cam0, which is ref)
    for frame:   poses of cal object


  measurements:
    for cameras:
      for frame:
        observations


- sfm

  just like calibration, but I have Nframes cameras, sparsely observing one
  giant calibration object. I have only one frame

  ref coord system: cam0

  state:
    for frame: maybe 3d positions of points. Not required if only two
      cameras observe the point.
    for cameras: poses of cameras (except cam0, which is ref)

  measurements:
    for cameras:
      for frame:
        observations
 */


// These are parameter variable scales. They have the units of the parameters
// themselves, so the optimizer sees x/SCALE_X for each parameter. I.e. as far
// as the optimizer is concerned, the scale of each variable is 1. This doesn't
// need to be precise; just need to get all the variables to be within the same
// order of magnitute. This is important because the dogleg solve treats the
// trust region as a ball in state space, and this ball is isotrophic, and has a
// radius that applies in every direction
#define SCALE_INTRINSICS_FOCAL_LENGTH 500.0
#define SCALE_INTRINSICS_CENTER_PIXEL 20.0
#define SCALE_ROTATION_CAMERA         (0.1 * M_PI/180.0)
#define SCALE_TRANSLATION_CAMERA      1.0
#define SCALE_ROTATION_FRAME          (15.0 * M_PI/180.0)
#define SCALE_TRANSLATION_FRAME       100.0
#define SCALE_POSITION_POINT          SCALE_TRANSLATION_FRAME
#define SCALE_CALOBJECT_WARP          0.01

#define DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M 1.0

// I need to constrain the point motion since it's not well-defined, and can
// jump to clearly-incorrect values. This is the distance in front of camera0. I
// make sure this is positive and not unreasonably high
#define POINT_MAXZ                    50000

// This is hard-coded to 1.0; the computation of scale_distortion_regularization
// below assumes it
#define SCALE_DISTORTION              1.0

#define MSG(fmt, ...) fprintf(stderr, "%s(%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define MSG_IF_VERBOSE(...) do { if(verbose) MSG( __VA_ARGS__ ); } while(0)



#define CHECK_CONFIG_NPARAM_NOCONFIG(s,n) \
    static_assert(n > 0, "no-config implies known-at-compile-time param count");
#define CHECK_CONFIG_NPARAM_WITHCONFIG(s,n) \
    static_assert(n <= 0, "with-config implies unknown-at-compile-time param count");
LENSMODEL_NOCONFIG_LIST(  CHECK_CONFIG_NPARAM_NOCONFIG)
LENSMODEL_WITHCONFIG_LIST(CHECK_CONFIG_NPARAM_WITHCONFIG)


// Returns a static string, using "..." as a placeholder for any configuration
// values
const char* mrcal_lensmodel_name( lensmodel_t model )
{
    switch(model.type)
    {
#define CASE_STRING_NOCONFIG(s,n) case s: ;                             \
        return #s;
#define CASE_STRING_WITHCONFIG(s,n) case s: ;                           \
        return #s "_...";

        LENSMODEL_NOCONFIG_LIST(   CASE_STRING_NOCONFIG )
        LENSMODEL_WITHCONFIG_LIST( CASE_STRING_WITHCONFIG )

    default:
        assert(0);


#undef CASE_STRING_NOCONFIG
#undef CASE_STRING_WITHCONFIG

    }
    return NULL;
}

// Write the model name WITH the full config into the given buffer. Identical to
// mrcal_lensmodel_name() for configuration-free models
static int LENSMODEL_SPLINED_STEREOGRAPHIC__snprintf_model
  (char* out, int size,
   const LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config)
{
    return
        snprintf( out, size, "LENSMODEL_SPLINED_STEREOGRAPHIC_%"PRIu16"_%"PRIu16"_%"PRIu16"_%"PRIu16"_%f_%f",
                  config->spline_order,
                  config->Nx, config->Ny,
                  config->fov_x_deg,
                  config->cx, config->cy);
}
bool mrcal_lensmodel_name_full( char* out, int size, lensmodel_t model )
{
    switch(model.type)
    {
#define CASE_STRING_NOCONFIG(s,n) case s: \
        return size > snprintf(out,size, #s);

#define CASE_STRING_WITHCONFIG(s,n) case s: \
        return size > s##__snprintf_model(out, size, &model.s##__config);

        LENSMODEL_NOCONFIG_LIST(   CASE_STRING_NOCONFIG )
        LENSMODEL_WITHCONFIG_LIST( CASE_STRING_WITHCONFIG )

    default:
        assert(0);

#undef CASE_STRING_NOCONFIG
#undef CASE_STRING_WITHCONFIG

    }
    return NULL;
}


static bool LENSMODEL_SPLINED_STEREOGRAPHIC__scan_model_config( LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config, const char* config_str)
{
    int pos;
    return
        6 == sscanf( config_str, "%"SCNu16"_%"SCNu16"_%"SCNu16"_%"SCNu16"_%f_%f%n",
                     &config->spline_order,
                     &config->Nx, &config->Ny,
                     &config->fov_x_deg,
                     &config->cx, &config->cy,
                     &pos) &&
        config_str[pos] == '\0';
}

lensmodel_t mrcal_lensmodel_from_name( const char* name )
{

#define CHECK_AND_RETURN_NOCONFIG(s,n)                                  \
    if( 0 == strcmp( name, #s) )                                        \
        return (lensmodel_t){.type = s};

#define CHECK_AND_RETURN_WITHCONFIG(s,n)                                \
    /* Configured model. I need to extract the config from the string. */ \
    /* The string format is NAME_cfg1_cfg2 ... */                       \
    const int name_len = strlen(#s);                                    \
    if( 0 == strcmp( name, #s) )                                        \
        return (lensmodel_t){.type = LENSMODEL_INVALID_BADCONFIG};      \
    if( 0 == strncmp( name, #s"_", name_len+1) )                        \
    {                                                                   \
        /* found name. Now extract the config */                        \
        lensmodel_t model = {.type = s};                                \
        s##__config_t* config = &model.s##__config;                     \
                                                                        \
        const char* config_str = &name[name_len+1];                     \
                                                                        \
        if(s##__scan_model_config(config, config_str))                  \
            return model;                                               \
        else                                                            \
            return (lensmodel_t){.type = LENSMODEL_INVALID_BADCONFIG};  \
    }

    LENSMODEL_NOCONFIG_LIST(   CHECK_AND_RETURN_NOCONFIG );
    LENSMODEL_WITHCONFIG_LIST( CHECK_AND_RETURN_WITHCONFIG );

    return (lensmodel_t){.type = LENSMODEL_INVALID};

#undef CHECK_AND_RETURN_NOCONFIG
#undef CHECK_AND_RETURN_WITHCONFIG
}

bool mrcal_modelHasCore_fxfycxcy( const lensmodel_t m )
{
    if(LENSMODEL_IS_OPENCV(m.type)) return true;
    if(LENSMODEL_IS_CAHVOR(m.type)) return true;
    if(m.type == LENSMODEL_PINHOLE) return true;

    if(m.type == LENSMODEL_SPLINED_STEREOGRAPHIC)
        return false;

    MSG("I don't know if %s has a core or not. Add that information to this function",
        mrcal_lensmodel_name(m));
    exit(1);
}

static int LENSMODEL_SPLINED_STEREOGRAPHIC__getNlensParams(const LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config)
{
    // I have two surfaces: one for x and another for y
    return (int)config->Nx * (int)config->Ny * 2;
}
int mrcal_getNlensParams(const lensmodel_t m)
{
    switch(m.type)
    {
#define CASE_NUM_NOCONFIG(s,n)                                          \
        case s: return n;

#define CASE_NUM_WITHCONFIG(s,n)                                        \
        case s: return s##__getNlensParams(&m.s##__config);

        LENSMODEL_NOCONFIG_LIST(   CASE_NUM_NOCONFIG )
        LENSMODEL_WITHCONFIG_LIST( CASE_NUM_WITHCONFIG )

    default: ;
    }
    return -1;

#undef CASE_NUM_NOCONFIG
#undef CASE_NUM_WITHCONFIG
}

const char* const* mrcal_getSupportedLensModels( void )
{
#define NAMESTRING_NOCONFIG(s,n)   #s,
#define NAMESTRING_WITHCONFIG(s,n) #s"_...",
    static const char* names[] = {
        LENSMODEL_NOCONFIG_LIST(  NAMESTRING_NOCONFIG)
        LENSMODEL_WITHCONFIG_LIST(NAMESTRING_WITHCONFIG)
        NULL };
    return names;
}

// Returns the 'next' lens model in a family
//
// In a family of lens models we have a sequence of models with increasing
// complexity. Subsequent models add distortion parameters to the end of the
// vector. Ealier models are identical, but with the extra paramaters set to 0.
// This function returns the next model in a sequence.
//
// If this is the last model in the sequence, returns the current model. This
// function takes in both the current model, and the last model we're aiming
// for. The second part is required because all familie begin at
// LENSMODEL_PINHOLE, so the next model from LENSMODEL_PINHOLE is not well-defined
// without more information
lensmodel_t mrcal_getNextLensModel( lensmodel_t lensmodel_now,
                                     lensmodel_t lensmodel_final )
{
    // if we're at the start of a sequence...
    if(lensmodel_now.type == LENSMODEL_PINHOLE)
    {
        if(LENSMODEL_IS_OPENCV(lensmodel_final.type)) return (lensmodel_t){.type=LENSMODEL_OPENCV4};
        if(LENSMODEL_IS_CAHVOR(lensmodel_final.type)) return (lensmodel_t){.type=LENSMODEL_CAHVOR};
        return (lensmodel_t){.type=LENSMODEL_INVALID};
    }

    // if we're at the end of a sequence...
    if(lensmodel_now.type == lensmodel_final.type)
        return lensmodel_now;

    // If there is no possible sequence, barf
    if(!LENSMODEL_IS_OPENCV(lensmodel_final.type) &&
       !LENSMODEL_IS_CAHVOR(lensmodel_final.type) )
        return (lensmodel_t){.type=LENSMODEL_INVALID};

    // I guess we're in the middle of a sequence
    return (lensmodel_t){.type=lensmodel_now.type+1};
}

static
int getNdistortionOptimizationParams(mrcal_problem_details_t problem_details,
                                     lensmodel_t lensmodel)
{
    if( !problem_details.do_optimize_intrinsic_distortions )
        return 0;

    int N = mrcal_getNlensParams(lensmodel);
    if(mrcal_modelHasCore_fxfycxcy(lensmodel))
        N -= 4; // ignoring fx,fy,cx,cy
    return N;
}

int mrcal_getNintrinsicOptimizationParams(mrcal_problem_details_t problem_details,
                                          lensmodel_t lensmodel)
{
    int N = getNdistortionOptimizationParams(problem_details, lensmodel);

    if( problem_details.do_optimize_intrinsic_core &&
        mrcal_modelHasCore_fxfycxcy(lensmodel) )
        N += 4; // fx,fy,cx,cy
    return N;
}

int mrcal_getNstate(int Ncameras, int Nframes, int Npoints,
                    mrcal_problem_details_t problem_details,
                    lensmodel_t lensmodel)
{
    return
        // camera extrinsics
        (problem_details.do_optimize_extrinsics ? ((Ncameras-1) * 6) : 0) +

        // frame poses, individual observed points
        (problem_details.do_optimize_frames ? (Nframes * 6 + Npoints * 3) : 0) +

        // camera intrinsics
        (Ncameras * mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel)) +

        // warp
        (problem_details.do_optimize_calobject_warp ? 2 : 0);
}

static int getNmeasurements_observationsonly(int NobservationsBoard,
                                             int NobservationsPoint,
                                             int calibration_object_width_n)
{
    // *2 because I have separate x and y measurements
    int Nmeas =
        NobservationsBoard *
        calibration_object_width_n*calibration_object_width_n *
        2;

    // *2 because I have separate x and y measurements
    Nmeas += NobservationsPoint * 2;
    return Nmeas;
}


static int LENSMODEL_SPLINED_STEREOGRAPHIC__getNregularizationTerms_percamera(const LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config)
{
    return 0;
}
static int getNregularizationTerms_percamera(mrcal_problem_details_t problem_details,
                                             lensmodel_t lensmodel)
{
    if(problem_details.do_skip_regularization)
        return 0;

    if(lensmodel.type == LENSMODEL_SPLINED_STEREOGRAPHIC)
        return LENSMODEL_SPLINED_STEREOGRAPHIC__getNregularizationTerms_percamera(&lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config);

    // distortions
    int N = getNdistortionOptimizationParams(problem_details, lensmodel);
    // optical center
    if(problem_details.do_optimize_intrinsic_core)
        N += 2;
    return N;
}

int mrcal_getNmeasurements_boards(int NobservationsBoard,
                                  int calibration_object_width_n)
{
    // *2 because I have separate x and y measurements
    return
        NobservationsBoard *
        calibration_object_width_n*calibration_object_width_n *
        2;
}

int mrcal_getNmeasurements_points(const observation_point_t* observations_point,
                                  int NobservationsPoint)
{
    // *2 because I have separate x and y measurements
    int Nmeas = NobservationsPoint * 2;

    // known-distance measurements
    for(int i=0; i<NobservationsPoint; i++)
        if(observations_point[i].dist > 0.0) Nmeas++;
    return Nmeas;
}

int mrcal_getNmeasurements_regularization(int Ncameras,
                                          mrcal_problem_details_t problem_details,
                                          lensmodel_t lensmodel)
{
    return
        Ncameras *
        getNregularizationTerms_percamera(problem_details, lensmodel);
}

int mrcal_getNmeasurements_all(int Ncameras, int NobservationsBoard,
                               const observation_point_t* observations_point,
                               int NobservationsPoint,
                               int calibration_object_width_n,
                               mrcal_problem_details_t problem_details,
                               lensmodel_t lensmodel)
{
    return
        mrcal_getNmeasurements_boards( NobservationsBoard, calibration_object_width_n) +
        mrcal_getNmeasurements_points( observations_point, NobservationsPoint) +
        mrcal_getNmeasurements_regularization( Ncameras, problem_details, lensmodel);
}

int mrcal_getN_j_nonzero( int Ncameras,
                          const observation_board_t* observations_board,
                          int NobservationsBoard,
                          const observation_point_t* observations_point,
                          int NobservationsPoint,
                          mrcal_problem_details_t problem_details,
                          lensmodel_t lensmodel,
                          int calibration_object_width_n)
{

    if(lensmodel.type == LENSMODEL_SPLINED_STEREOGRAPHIC)
    {
        #warning do thing
    }


    // each observation depends on all the parameters for THAT frame and for
    // THAT camera. Camera0 doesn't have extrinsics, so I need to loop through
    // all my observations

    // initial estimate counts extrinsics for camera0, which need to be
    // subtracted off
    int Nintrinsics = mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel);

    // x depends on fx,cx but NOT on fy, cy. And similarly for y.
    if( problem_details.do_optimize_intrinsic_core &&
        mrcal_modelHasCore_fxfycxcy(lensmodel) )
        Nintrinsics -= 2;

    int N = NobservationsBoard * ( (problem_details.do_optimize_frames         ? 6 : 0) +
                                   (problem_details.do_optimize_extrinsics     ? 6 : 0) +
                                   (problem_details.do_optimize_calobject_warp ? 2 : 0) +
                                   + Nintrinsics );
    if(problem_details.do_optimize_extrinsics)
        for(int i=0; i<NobservationsBoard; i++)
            if(observations_board[i].i_camera == 0)
                N -= 6;
    N *= 2*calibration_object_width_n*calibration_object_width_n; // *2 because I have separate x and y measurements

    // Now the point observations
    for(int i=0; i<NobservationsPoint; i++)
    {
        N += 2*Nintrinsics;
        if(problem_details.do_optimize_frames)
            N += 2*3;
        if( problem_details.do_optimize_extrinsics &&
            observations_point[i].i_camera != 0 )
            N += 2*6;

        if(observations_point[i].dist > 0)
        {
            if(problem_details.do_optimize_frames)
                N += 3;

            if( problem_details.do_optimize_extrinsics &&
                observations_point[i].i_camera != 0 )
                N += 6;
        }
    }

    N +=
        Ncameras *
        getNregularizationTerms_percamera(problem_details,
                                          lensmodel);

    return N;
}

// Used in the spline-based projection function.
//
// See bsplines.py for the derivation of the spline expressions and for
// justification of the 2D scheme
//
// Here we sample two interpolated surfaces at once: one each for the x and y
// focal-length scales
static
void sample_bspline_surface_cubic(double* out,
                                  double* dout_dx,
                                  double* dout_dy,
                                  double* ABCDx, double* ABCDy,

                                  double x, double y,
                                  // control points
                                  const double* c,
                                  int stridey

                                  // stridex is 2: the control points from the
                                  // two surfaces are next to each other. Better
                                  // cache locality maybe
                                  )
{
    // The sampling function assumes evenly spaced knots.
    // a,b,c,d are sequential control points
    // x is in [0,1] between b and c. Function looks like this:
    //   double x2 = x*x;
    //   double x3 = x2*x;
    //   double A =  (-x3 + 3*x2 - 3*x + 1)/6;
    //   double B = (3 * x3/2 - 3*x2 + 2)/3;
    //   double C = (-3 * x3 + 3*x2 + 3*x + 1)/6;
    //   double D = (x * x * x) / 6;
    //   return A*a0 + B*b0 + C*c0 + D*d0;
    // I need to sample many such 1D segments, so I compute A,B,C,D separately,
    // and apply them together
    void get_sample_coeffs(double* ABCD, double* ABCDgrad, double x)
    {
        double x2 = x*x;
        double x3 = x2*x;
        ABCD[0] =  (-x3 + 3*x2 - 3*x + 1)/6;
        ABCD[1] = (3 * x3/2 - 3*x2 + 2)/3;
        ABCD[2] = (-3 * x3 + 3*x2 + 3*x + 1)/6;
        ABCD[3] = x3 / 6;

        ABCDgrad[0] =  -x2/2 + x - 1./2.;
        ABCDgrad[1] = 3*x2/2 - 2*x;
        ABCDgrad[2] = -3*x2/2 + x + 1./2.;
        ABCDgrad[3] = x2 / 2;
    }

    // 4 samples along one dimension, and then one sample along the other
    // dimension, using the 4 samples as the control points. Order doesn't
    // matter. See bsplines.py
    //
    // I do this twice: one for each focal length surface
    double ABCDgradx[4];
    double ABCDgrady[4];
    get_sample_coeffs(ABCDx, ABCDgradx, x);
    get_sample_coeffs(ABCDy, ABCDgrady, y);
    double cinterp[4][2];
    const int stridex = 2;

    void interp(double* out, const double* ABCDx, const double* ABCDy)
    {
        for(int iy=0; iy<4; iy++)
            for(int k=0;k<2;k++)
                cinterp[iy][k] =
                    ABCDx[0] * c[iy*stridey + 0*stridex + k] +
                    ABCDx[1] * c[iy*stridey + 1*stridex + k] +
                    ABCDx[2] * c[iy*stridey + 2*stridex + k] +
                    ABCDx[3] * c[iy*stridey + 3*stridex + k];
        for(int k=0;k<2;k++)
            out[k] =
                ABCDy[0] * cinterp[0][k] +
                ABCDy[1] * cinterp[1][k] +
                ABCDy[2] * cinterp[2][k] +
                ABCDy[3] * cinterp[3][k];
    }

    // the intrinsics gradient is flatten(ABCDx[0..3] * ABCDy[0..3]) for both x
    // and y. By returning ABCD[xy] and not the cartesian products, I make
    // smaller temporary data arrays
    interp(out,     ABCDx,     ABCDy);
    interp(dout_dx, ABCDgradx, ABCDy);
    interp(dout_dy, ABCDx,     ABCDgrady);
}

typedef struct
{
#warning some of these are almost certainly zero
    double _d_rj_rf[3*3];
    double _d_rj_tf[3*3];
    double _d_rj_rc[3*3];
    double _d_rj_tc[3*3];
    double _d_tj_rf[3*3];
    double _d_tj_tf[3*3];
    double _d_tj_rc[3*3];
    double _d_tj_tc[3*3];
} geometric_gradients_t;

static void project_opencv( // out
                           point2_t* restrict pt_out,

                           // dx/dfx and dy/dfy. One entry per point (no cross
                           // terms). NULL if not needed
                           double*   restrict dxy_dfxy,

                           // No d/dcx terms. Assumed to be the identity

                           // Array of shape (Npoints,2,Ndistortions). NULL if
                           // not needed
                           double*   restrict dxy_dintrinsics_nocore,

                           point3_t* restrict dxy_drcamera,
                           point3_t* restrict dxy_dtcamera,
                           point3_t* restrict dxy_drframe,
                           point3_t* restrict dxy_dtframe,
                           point2_t* restrict dxy_dcalobject_warp,

                           // in

                           const double* restrict intrinsics,
                           const point2_t* restrict calobject_warp,

                           // point index. If <0, a point at frame_rt->t is
                           // assumed; frame_rt->r isn't referenced, and
                           // dxy_drframe is expected to be NULL. And the
                           // calibration_object_... variables aren't used either

                           double calibration_object_spacing,
                           int    calibration_object_width_n,

                           // stuff from project()
                           int NdistortionParams,

                           // if NULL then the camera is at the reference
                           const geometric_gradients_t* gg,
                           CvMat* p_rj,
                           CvMat* p_tj)

{
    const int Npoints =
        calibration_object_width_n ?
        calibration_object_width_n*calibration_object_width_n : 1;

    point3_t pt_ref        [Npoints];
    point2_t dpt_ref2_dwarp[Npoints];
    memset(pt_ref,         0, Npoints * sizeof(pt_ref[0]));
    memset(dpt_ref2_dwarp, 0, Npoints * sizeof(dpt_ref2_dwarp[0]));
    if(calibration_object_width_n)
    {
        int i_pt = 0;

        // The calibration object has a simple grid geometry
        for(int y = 0; y<calibration_object_width_n; y++)
            for(int x = 0; x<calibration_object_width_n; x++)
            {
                pt_ref[i_pt].x = (double)x * calibration_object_spacing;
                pt_ref[i_pt].y = (double)y * calibration_object_spacing;
                // pt_ref[i_pt].z = 0.0; This is already done

                if(calobject_warp != NULL)
                {
                    // Add a board warp here. I have two parameters, and they describe
                    // additive flex along the x axis and along the y axis, in that
                    // order. In each direction the flex is a parabola, with the
                    // parameter k describing the max deflection at the center. If the
                    // ends are at +- 1 I have d = k*(1 - x^2). If the ends are at
                    // (0,N-1) the equivalent expression is: d = k*( 1 - 4*x^2/(N-1)^2 +
                    // 4*x/(N-1) - 1 ) = d = 4*k*(x/(N-1) - x^2/(N-1)^2) = d =
                    // 4.*k*x*r(1. - x*r)
                    double r = 1./(double)(calibration_object_width_n-1);

                    double xr = (double)x * r;
                    double yr = (double)y * r;
                    double dx = 4. * xr * (1. - xr);
                    double dy = 4. * yr * (1. - yr);
                    pt_ref[i_pt].z += calobject_warp->x * dx;
                    pt_ref[i_pt].z += calobject_warp->y * dy;
                    dpt_ref2_dwarp[i_pt].x = dx;
                    dpt_ref2_dwarp[i_pt].y = dy;
                }

                i_pt++;
            }
    }
    else
    {
        // We're not looking at a calibration board point, but rather a
        // standalone point. I leave pt_ref at the origin, and take the
        // coordinate from frame_rt->t
    }

    // OpenCV does the projection AND the gradient propagation for me, so I
    // implement a separate code path for it
    CvMat object_points  = cvMat(3,Npoints, CV_64FC1, pt_ref[0].xyz);
    CvMat image_points   = cvMat(2,Npoints, CV_64FC1, pt_out[0].xy);

    // I compute these even if I'm not going to use them. This is a potential
    // optimization spot
    double _dxy_drj[2*Npoints*3];
    double _dxy_dtj[2*Npoints*3];
    CvMat  dxy_drj = cvMat(2*Npoints,3, CV_64FC1, _dxy_drj);
    CvMat  dxy_dtj = cvMat(2*Npoints,3, CV_64FC1, _dxy_dtj);

    double fx = intrinsics[0];
    double fy = intrinsics[1];
    double cx = intrinsics[2];
    double cy = intrinsics[3];

    double _camera_matrix[] = {
        fx,  0, cx,
        0,  fy, cy,
        0,   0,  1 };
    CvMat camera_matrix = cvMat(3,3, CV_64FC1, _camera_matrix);
    CvMat _distortions = cvMat( NdistortionParams, 1, CV_64FC1,
                                // removing const, but that's just because
                                // OpenCV's API is incomplete. It IS const
                                (double*)&intrinsics[4]);

    if( dxy_dfxy == NULL )
    {
        cvProjectPoints2(&object_points,
                         p_rj, p_tj,
                         &camera_matrix,
                         &_distortions,
                         &image_points,
                         &dxy_drj, &dxy_dtj,
                         NULL, NULL,
                         dxy_dintrinsics_nocore ?
                           (CvMat[]){cvMat(2*Npoints, NdistortionParams, CV_64FC1, dxy_dintrinsics_nocore)} :
                           NULL,
                         0 );
    }
    else
    {
        // Opencv returns dpdf as a full dp/df 2x2 matrix per point, while I
        // assume that the off-diagonal entries are 0 and return the diagonal
        // only. So I need to make a new memory buffer, and to masage
        double _dpdf[2*Npoints*2];
        CvMat dpdf = cvMat(2*Npoints,2, CV_64FC1, _dpdf);

        cvProjectPoints2(&object_points,
                         p_rj, p_tj,
                         &camera_matrix,
                         &_distortions,
                         &image_points,
                         &dxy_drj, &dxy_dtj,
                         &dpdf,
                         NULL, // dp_dc is trivial: it's the identity
                         dxy_dintrinsics_nocore ?
                           (CvMat[]){cvMat(2*Npoints, NdistortionParams, CV_64FC1, dxy_dintrinsics_nocore)} :
                           NULL,
                         0 );

        for(int i=0; i<Npoints; i++)
        {
            dxy_dfxy[2*i + 0] = _dpdf[4*i + 0]; // dx/dfx
            dxy_dfxy[2*i + 1] = _dpdf[4*i + 3]; // dy/dfy
            if(_dpdf[4*i + 1] != 0.0 || _dpdf[4*i + 2] != 0.0)
                MSG("WARNING: Opencv returned non-zero off-diagonal term in dp/dfxy. This shouldn't happen. Assuming it WAS zero");
        }
    }

    if( dxy_drcamera != NULL || dxy_drframe != NULL ||
        dxy_dtcamera != NULL || dxy_dtframe != NULL )
    {
        void set_dxy_drtframe(int i_pt)
        {
            if(gg != NULL)
            {
                // I do this multiple times, one each for {r,t}{camera,frame}
                void propagate(// out
                               point3_t* dxy_dparam,

                               // in
                               const double* _d_rj_dparam,
                               const double* _d_tj_dparam)
                {
                    if( dxy_dparam == NULL ) return;

                    // I have dproj/drj and dproj/dtj
                    // I want dproj/drc, dproj/dtc, dproj/drf, dprof/dtf
                    //
                    // dproj_drc = dproj/drj drj_drc + dproj/dtj dtj_drc

                    mul_genN3_gen33_vout  (2, &_dxy_drj[i_pt*2*3], _d_rj_dparam, dxy_dparam[i_pt*2].xyz);
                    mul_genN3_gen33_vaccum(2, &_dxy_dtj[i_pt*2*3], _d_tj_dparam, dxy_dparam[i_pt*2].xyz);
                }

                propagate( dxy_drcamera, gg->_d_rj_rc, gg->_d_tj_rc );
                propagate( dxy_dtcamera, gg->_d_rj_tc, gg->_d_tj_tc );
                propagate( dxy_dtframe , gg->_d_rj_tf, gg->_d_tj_tf );
                propagate( dxy_drframe , gg->_d_rj_rf, gg->_d_tj_rf );
            }
            else
            {
                // My gradient is already computed. Copy it
                if(dxy_dtframe)
                {
                    memcpy(dxy_dtframe[i_pt*2 + 0].xyz, &_dxy_dtj[i_pt*2*3 + 3*0], 3*sizeof(double));
                    memcpy(dxy_dtframe[i_pt*2 + 1].xyz, &_dxy_dtj[i_pt*2*3 + 3*1], 3*sizeof(double));
                }
                if(dxy_drframe)
                {
                    memcpy(dxy_drframe[i_pt*2 + 0].xyz, &_dxy_drj[i_pt*2*3 + 3*0], 3*sizeof(double));
                    memcpy(dxy_drframe[i_pt*2 + 1].xyz, &_dxy_drj[i_pt*2*3 + 3*1], 3*sizeof(double));
                }
            }
        }

        if(calibration_object_width_n)
            for(int i_pt = 0;
                i_pt<calibration_object_width_n*calibration_object_width_n;
                i_pt++)
                set_dxy_drtframe(i_pt);
        else
            set_dxy_drtframe(0);
    }

    if( dxy_dcalobject_warp != NULL && calibration_object_width_n )
    {
        // p = proj(R( warp(x) ) + t);
        // dp/dw = dp/dR(warp(x)) dR(warp(x))/dwarp(x) dwarp/dw =
        //       = dp/dt R dwarp/dw
        // dp/dt is _dxy_dtj
        // R is rodrigues(rj)
        // dwarp/dw = [0 0]
        //            [0 0]
        //            [a b]
        // Let R = [r0 r1 r2]
        // dp/dw = dp/dt [ar2 br2] = [a dp/dt r2    b dp/dt r2]

        double _Rj[3*3];
        CvMat  Rj = cvMat(3,3,CV_64FC1, _Rj);
        cvRodrigues2(p_rj, &Rj, NULL);

        for(int i_pt = 0;
            i_pt<calibration_object_width_n*calibration_object_width_n;
            i_pt++)
        {
            double d[] =
                { _dxy_dtj[i_pt*2*3 + 3*0 + 0] * _Rj[0*3 + 2] +
                  _dxy_dtj[i_pt*2*3 + 3*0 + 1] * _Rj[1*3 + 2] +
                  _dxy_dtj[i_pt*2*3 + 3*0 + 2] * _Rj[2*3 + 2],
                  _dxy_dtj[i_pt*2*3 + 3*1 + 0] * _Rj[0*3 + 2] +
                  _dxy_dtj[i_pt*2*3 + 3*1 + 1] * _Rj[1*3 + 2] +
                  _dxy_dtj[i_pt*2*3 + 3*1 + 2] * _Rj[2*3 + 2]};

            dxy_dcalobject_warp[i_pt*2 + 0].x = d[0]*dpt_ref2_dwarp[i_pt].x;
            dxy_dcalobject_warp[i_pt*2 + 0].y = d[0]*dpt_ref2_dwarp[i_pt].y;
            dxy_dcalobject_warp[i_pt*2 + 1].x = d[1]*dpt_ref2_dwarp[i_pt].x;
            dxy_dcalobject_warp[i_pt*2 + 1].y = d[1]*dpt_ref2_dwarp[i_pt].y;
        }
    }
}

// These are all internals for project(). It was getting unwieldy otherwise
static
void _project_point_parametric( // outputs
                               point2_t* pt_out,
                               double* p_dxy_dfxy, double* p_dxy_dintrinsics_nocore,
                               point3_t* restrict dxy_drcamera,
                               point3_t* restrict dxy_dtcamera,
                               point3_t* restrict dxy_drframe,
                               point3_t* restrict dxy_dtframe,
                               point2_t* restrict dxy_dcalobject_warp,

                               // inputs
                               const point3_t* pt_cam,
                               const double* dptcam_drc,
                               const double* dptcam_dtc,
                               const double* dptcam_drf,
                               const double* dptcam_dtf,

                               const double* restrict intrinsics,
                               bool camera_at_identity,
                               lensmodel_t lensmodel,
                               const point2_t* dpt_ref2_dwarp, int i_pt,
                               const double* _Rj)
{
    // u = distort(pt_cam, distortions)
    // q = uxy/uz * fxy + cxy
    int NdistortionParams = mrcal_getNlensParams(lensmodel) - 4;

    if( lensmodel.type == LENSMODEL_PINHOLE )
    {
        // q = fxy pxy/pz + cxy
        // dqx/dp = d( fx px/pz + cx ) = fx/pz^2 (pz [1 0 0] - px [0 0 1])
        // dqy/dp = d( fy py/pz + cy ) = fy/pz^2 (pz [0 1 0] - py [0 0 1])
        const double fx = intrinsics[0];
        const double fy = intrinsics[1];
        const double cx = intrinsics[2];
        const double cy = intrinsics[3];
        double pz_recip = 1. / pt_cam->z;
        pt_out[i_pt].x = pt_cam->x*pz_recip * fx + cx;
        pt_out[i_pt].y = pt_cam->y*pz_recip * fy + cy;

        double dq_dp[2][3] =
            { { fx * pz_recip,             0, -fx*pt_cam->x*pz_recip*pz_recip},
              { 0,             fy * pz_recip, -fy*pt_cam->y*pz_recip*pz_recip} };

        // dq/deee = dq/dp dp/deee
        if(camera_at_identity)
        {
            if( dxy_drcamera != NULL ) memset(dxy_drcamera[2*i_pt].xyz, 0, 6*sizeof(double));
            if( dxy_dtcamera != NULL ) memset(dxy_dtcamera[2*i_pt].xyz, 0, 6*sizeof(double));
            if( dxy_drframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dp, dptcam_drf, dxy_drframe[2*i_pt].xyz);
            if( dxy_dtframe  != NULL ) memcpy(dxy_dtframe[2*i_pt].xyz, dq_dp, 6*sizeof(double));
        }
        else
        {
            if( dxy_drcamera != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dp, dptcam_drc, dxy_drcamera[2*i_pt].xyz);
            if( dxy_dtcamera != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dp, dptcam_dtc, dxy_dtcamera[2*i_pt].xyz);
            if( dxy_drframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dp, dptcam_drf, dxy_drframe [2*i_pt].xyz);
            if( dxy_dtframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dp, dptcam_dtf, dxy_dtframe [2*i_pt].xyz);
        }

        // I have the projection, and I now need to propagate the gradients
        if( p_dxy_dfxy )
        {
            // I have the projection, and I now need to propagate the gradients
            // xy = fxy * distort(xy)/distort(z) + cxy
            p_dxy_dfxy[2*i_pt + 0] = pt_cam->x*pz_recip; // dx/dfx
            p_dxy_dfxy[2*i_pt + 1] = pt_cam->y*pz_recip; // dy/dfy
        }
    }
    else if( lensmodel.type == LENSMODEL_CAHVOR )
    {
        // I perturb pt_cam, and then apply the focal length, center pixel stuff
        // normally
        point3_t pt_cam_distorted;

        // distortion parameter layout:
        //   alpha
        //   beta
        //   r0
        //   r1
        //   r2
        double alpha = intrinsics[4 + 0];
        double beta  = intrinsics[4 + 1];
        double r0    = intrinsics[4 + 2];
        double r1    = intrinsics[4 + 3];
        double r2    = intrinsics[4 + 4];

        double s_al, c_al, s_be, c_be;
        sincos(alpha, &s_al, &c_al);
        sincos(beta,  &s_be, &c_be);

        // I parametrize the optical axis such that
        // - o(alpha=0, beta=0) = (0,0,1) i.e. the optical axis is at the center
        //   if both parameters are 0
        // - The gradients are cartesian. I.e. do/dalpha and do/dbeta are both
        //   NOT 0 at (alpha=0,beta=0). This would happen at the poles (gimbal
        //   lock), and that would make my solver unhappy
        double o     []         = {  s_al*c_be, s_be,  c_al*c_be };
        double do_dalpha[]      = {  c_al*c_be,    0, -s_al*c_be };
        double do_dbeta[]       = { -s_al*s_be, c_be, -c_al*s_be };

        double norm2p        = norm2_vec(3, pt_cam->xyz);
        double omega         = dot_vec(3, pt_cam->xyz, o);
        double domega_dalpha = dot_vec(3, pt_cam->xyz, do_dalpha);
        double domega_dbeta  = dot_vec(3, pt_cam->xyz, do_dbeta);

        double omega_recip = 1.0 / omega;
        double tau         = norm2p * omega_recip*omega_recip - 1.0;
        double s__dtau_dalphabeta__domega_dalphabeta = -2.0*norm2p * omega_recip*omega_recip*omega_recip;
        double dmu_dtau = r1 + 2.0*tau*r2;
        double dmu_dxyz[3];
        for(int i=0; i<3; i++)
            dmu_dxyz[i] = dmu_dtau *
                (2.0 * pt_cam->xyz[i] * omega_recip*omega_recip + s__dtau_dalphabeta__domega_dalphabeta * o[i]);
        double mu = r0 + tau*r1 + tau*tau*r2;
        double s__dmu_dalphabeta__domega_dalphabeta = dmu_dtau * s__dtau_dalphabeta__domega_dalphabeta;

        double  dptcamdistorted_dpcam[3*3] = {};
        double  dptcamdistorted_ddistortion[3*NdistortionParams];

        for(int i=0; i<3; i++)
        {
            double dmu_ddist[5] = { s__dmu_dalphabeta__domega_dalphabeta * domega_dalpha,
                s__dmu_dalphabeta__domega_dalphabeta * domega_dbeta,
                1.0,
                tau,
                tau * tau };

            dptcamdistorted_ddistortion[i*NdistortionParams + 0] = pt_cam->xyz[i] * dmu_ddist[0];
            dptcamdistorted_ddistortion[i*NdistortionParams + 1] = pt_cam->xyz[i] * dmu_ddist[1];
            dptcamdistorted_ddistortion[i*NdistortionParams + 2] = pt_cam->xyz[i] * dmu_ddist[2];
            dptcamdistorted_ddistortion[i*NdistortionParams + 3] = pt_cam->xyz[i] * dmu_ddist[3];
            dptcamdistorted_ddistortion[i*NdistortionParams + 4] = pt_cam->xyz[i] * dmu_ddist[4];

            dptcamdistorted_ddistortion[i*NdistortionParams + 0] -= dmu_ddist[0] * omega*o[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 1] -= dmu_ddist[1] * omega*o[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 2] -= dmu_ddist[2] * omega*o[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 3] -= dmu_ddist[3] * omega*o[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 4] -= dmu_ddist[4] * omega*o[i];

            dptcamdistorted_ddistortion[i*NdistortionParams + 0] -= mu * domega_dalpha*o[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 1] -= mu * domega_dbeta *o[i];

            dptcamdistorted_ddistortion[i*NdistortionParams + 0] -= mu * omega * do_dalpha[i];
            dptcamdistorted_ddistortion[i*NdistortionParams + 1] -= mu * omega * do_dbeta [i];

            dptcamdistorted_dpcam[3*i + i] = mu+1.0;
            for(int j=0; j<3; j++)
            {
                dptcamdistorted_dpcam[3*i + j] += (pt_cam->xyz[i] - omega*o[i]) * dmu_dxyz[j];
                dptcamdistorted_dpcam[3*i + j] -= mu*o[i]*o[j];
            }

            pt_cam_distorted.xyz[i] = pt_cam->xyz[i] + mu * (pt_cam->xyz[i] - omega*o[i]);
        }

        // q = fxy pxy/pz + cxy
        // dqx/dp = d( fx px/pz + cx ) = fx/pz^2 (pz [1 0 0] - px [0 0 1])
        // dqy/dp = d( fy py/pz + cy ) = fy/pz^2 (pz [0 1 0] - py [0 0 1])
        const double fx = intrinsics[0];
        const double fy = intrinsics[1];
        const double cx = intrinsics[2];
        const double cy = intrinsics[3];
        double pz_recip = 1. / pt_cam_distorted.z;
        pt_out[i_pt].x = pt_cam_distorted.x*pz_recip * fx + cx;
        pt_out[i_pt].y = pt_cam_distorted.y*pz_recip * fy + cy;

        double dq_dp[2][3] =
            { { fx * pz_recip,             0, -fx*pt_cam_distorted.x*pz_recip*pz_recip},
              { 0,             fy * pz_recip, -fy*pt_cam_distorted.y*pz_recip*pz_recip} };
        // This is for the DISTORTED p.
        // dq/deee = dq/dpdistorted dpdistorted/dpundistorted dpundistorted/deee

        double dq_dpundistorted[6];
        mul_genN3_gen33_vout(2, (double*)dq_dp, dptcamdistorted_dpcam, dq_dpundistorted);

        // dq/deee = dq/dp dp/deee
        if(camera_at_identity)
        {
            if( dxy_drcamera != NULL ) memset(dxy_drcamera[2*i_pt].xyz, 0, 6*sizeof(double));
            if( dxy_dtcamera != NULL ) memset(dxy_dtcamera[2*i_pt].xyz, 0, 6*sizeof(double));
            if( dxy_drframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dpundistorted, dptcam_drf, dxy_drframe[2*i_pt].xyz);
            if( dxy_dtframe  != NULL ) memcpy(dxy_dtframe[2*i_pt].xyz, dq_dpundistorted, 6*sizeof(double));
        }
        else
        {
            if( dxy_drcamera != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dpundistorted, dptcam_drc, dxy_drcamera[2*i_pt].xyz);
            if( dxy_dtcamera != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dpundistorted, dptcam_dtc, dxy_dtcamera[2*i_pt].xyz);
            if( dxy_drframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dpundistorted, dptcam_drf, dxy_drframe [2*i_pt].xyz);
            if( dxy_dtframe  != NULL ) mul_genN3_gen33_vout(2, (double*)dq_dpundistorted, dptcam_dtf, dxy_dtframe [2*i_pt].xyz);
        }

        if( p_dxy_dintrinsics_nocore != NULL )
        {
            for(int i=0; i<NdistortionParams; i++)
            {
                const double dx = dptcamdistorted_ddistortion[i + 0*NdistortionParams];
                const double dy = dptcamdistorted_ddistortion[i + 1*NdistortionParams];
                const double dz = dptcamdistorted_ddistortion[i + 2*NdistortionParams];
                p_dxy_dintrinsics_nocore[(2*i_pt + 0)*NdistortionParams + i ] = fx * pz_recip * (dx - pt_cam_distorted.x*pz_recip*dz);
                p_dxy_dintrinsics_nocore[(2*i_pt + 1)*NdistortionParams + i ] = fy * pz_recip * (dy - pt_cam_distorted.y*pz_recip*dz);
            }
        }

        if( p_dxy_dfxy )
        {
            // I have the projection, and I now need to propagate the gradients
            // xy = fxy * distort(xy)/distort(z) + cxy
            p_dxy_dfxy[2*i_pt + 0] = pt_cam_distorted.x*pz_recip; // dx/dfx
            p_dxy_dfxy[2*i_pt + 1] = pt_cam_distorted.y*pz_recip; // dy/dfy
        }
    }
    else
    {
        MSG("Unhandled lens model: %d (%s)",
            lensmodel.type, mrcal_lensmodel_name(lensmodel));
        assert(0);
    }

    if( dxy_dcalobject_warp != NULL && dpt_ref2_dwarp != NULL )
    {
        // p = proj(Rc Rf warp(x) + Rc tf + tc);
        // dp/dw = dp/dRcRf(warp(x)) dR(warp(x))/dwarp(x) dwarp/dw =
        //       = dp/dtc RcRf dwarp/dw
        // dp/dtc is dxy_dtcamera
        // R is rodrigues(rj)
        // dwarp/dw = [0 0]
        //            [0 0]
        //            [a b]
        // Let R = [r0 r1 r2]
        // dp/dw = dp/dt [ar2 br2] = [a dp/dt r2    b dp/dt r2]
        point3_t* p_dxy_dt;
        if(!camera_at_identity) p_dxy_dt = &dxy_dtcamera[2*i_pt];
        else                    p_dxy_dt = &dxy_dtframe[2*i_pt];
        point3_t dxy_dt_forwarp[2];
        if(!p_dxy_dt)
        {
            MSG("we were asked for the calobject gradient, but not the tframe gradient. this isn't supported");
            assert(0);
        }
        double d[] =
            { p_dxy_dt[0].xyz[0] * _Rj[0*3 + 2] +
              p_dxy_dt[0].xyz[1] * _Rj[1*3 + 2] +
              p_dxy_dt[0].xyz[2] * _Rj[2*3 + 2],
              p_dxy_dt[1].xyz[0] * _Rj[0*3 + 2] +
              p_dxy_dt[1].xyz[1] * _Rj[1*3 + 2] +
              p_dxy_dt[1].xyz[2] * _Rj[2*3 + 2]};

        dxy_dcalobject_warp[2*i_pt + 0].x = d[0]*dpt_ref2_dwarp->x;
        dxy_dcalobject_warp[2*i_pt + 0].y = d[0]*dpt_ref2_dwarp->y;
        dxy_dcalobject_warp[2*i_pt + 1].x = d[1]*dpt_ref2_dwarp->x;
        dxy_dcalobject_warp[2*i_pt + 1].y = d[1]*dpt_ref2_dwarp->y;
    }
}


typedef struct
{
    point2_t u;
    double ABCDx[4];
    double ABCDy[4];
} splined_intrinsics_grad_context_t;

static
void _project_point_splined( // outputs
                            point2_t* pt_out,

                            splined_intrinsics_grad_context_t* ctx,
                            int* ivar0,

                            // Gradient outputs. May be NULL
                            point3_t* restrict dxy_drcamera,
                            point3_t* restrict dxy_dtcamera,
                            point3_t* restrict dxy_drframe,
                            point3_t* restrict dxy_dtframe,
                            point2_t* restrict dxy_dcalobject_warp,

                            // inputs
                            const point3_t* pt_cam,
                            const double* dptcam_drc,
                            const double* dptcam_dtc,
                            const double* dptcam_drf,
                            const double* dptcam_dtf,

                            const double* restrict intrinsics,
                            bool camera_at_identity,
                            lensmodel_t lensmodel,
                            const point2_t* dpt_ref2_dwarp, int i_pt,
                            const double* _Rj)
{
    if(lensmodel.type != LENSMODEL_SPLINED_STEREOGRAPHIC)
    {
        MSG("Unhandled lens model: %d (%s)",
            lensmodel.type,
            mrcal_lensmodel_name(lensmodel));
        assert(0);
    }

     const LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config =
        &lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config;

    // stereographic projection:
    //   (from https://en.wikipedia.org/wiki/Fisheye_lens)
    //   u = xy_unit * tan(th/2) * 2
    //
    // I compute the normalized (focal-length = 1) projection, and
    // use that to look-up the x and y focal length scalings

    // th is the angle between the observation and the projection
    // center
    //
    // sin(th)   = mag_xy/mag_xyz
    // cos(th)   = z/mag_xyz
    // tan(th/2) = sin(th) / (1 + cos(th))

    // tan(th/2) = mag_xy/mag_xyz / (1 + z/mag_xyz) =
    //           = mag_xy / (mag_xyz + z)
    // u = xy_unit * tan(th/2) * 2 =
    //   = xy/mag_xy * mag_xy/(mag_xyz + z) * 2 =
    //   = xy / (mag_xyz + z) * 2
    double mag_xyz = sqrt( pt_cam->x*pt_cam->x +
                           pt_cam->y*pt_cam->y +
                           pt_cam->z*pt_cam->z );
    double scale = 2.0 / (mag_xyz + pt_cam->z);

    ctx->u = (point2_t){.x = pt_cam->x * scale,
                        .y = pt_cam->y * scale};
    // du/dp = d/dp ( xy * scale )
    //       = pxy dscale/dp + [I; 0]
    // dscale/dp = d (2.0 / (mag_xyz + pt_cam->z))/dp =
    //           = -2/()^2 ( [0,0,1] + dmag/dp)
    //           = -2/()^2 ( [0,0,1] + 2pt/2mag)
    //           = -2 scale^2/4 ( [0,0,1] + pt/mag)
    //           = -scale^2/2 * ( [0,0,1] + pt/mag )
    //           = A * ([0,0,1] + pt B)
    double A = -scale*scale / 2.;
    double B = 1.0 / mag_xyz;
    double du_dp[2][3] = { { pt_cam->x *  A * B * pt_cam->x + 1,
                             pt_cam->x *  A * B * pt_cam->y,
                             pt_cam->x * (A * B * pt_cam->z + A) },
                           { pt_cam->y *  A * B * pt_cam->x,
                             pt_cam->y *  A * B * pt_cam->y + 1,
                             pt_cam->y * (A * B * pt_cam->z + A) } };

    //MSG("normalized projection: %f %f", ctx->u.x, ctx->u.y);

    // Great. I have the normalized projection. I use this to look
    // up the focal length. And I apply that focal length to this
    // normalized projection
    //
    // I have N control points describing a given field-of-view. I
    // want to space out the control points evenly. I'm using
    // B-splines, so I need extra control points out past my edge.
    // With cubic splines I need a whole extra interval past the
    // edge. With quadratic splines I need half an interval (see
    // show-fisheye-grid.py).
    //
    // (width + k*interval_size)/(N-1) = interval_size
    // ---> width/(N-1) = interval_size * (1 - k/(N-1))
    // ---> interval_size = width / (N - 1 - k)
    int NextraIntervals;
    switch(config->spline_order)
    {
#warning need to support quadratic splines
        // case 2: NextraIntervals = 1; break;
    case 3: NextraIntervals = 2; break;
    default:
        MSG("I only support spline_order 2 and 3");
        assert(0);
    }
    if(config->Nx < 4 || config->Ny < 4)
    {
        MSG("I only support Nx and Ny must be >= 4");
        assert(0);
    }
#warning this fov stuff can be done once per model, not for each projection
    double th_fov_x_edge = (double)config->fov_x_deg/2. * M_PI / 180.;
    double q_edge_x      = tan(th_fov_x_edge / 2.) * 2;
    double interval_size = (q_edge_x*2.) / (config->Nx - 1 - NextraIntervals);
    double ix = ctx->u.x/interval_size + (double)(config->Nx-1)/2.;
    double iy = ctx->u.y/interval_size + (double)(config->Ny-1)/2.;
#warning need to bounds-check
    int ix0 = (int)ix;
    int iy0 = (int)iy;

    //MSG("spline grid coords:: %f %f", ix,iy);

    point2_t fxy;
    double dfxy_dux[2];
    double dfxy_duy[2];
    *ivar0 = 2*( (iy0-1)*config->Nx +
                 (ix0-1) );
    sample_bspline_surface_cubic(fxy.xy, dfxy_dux, dfxy_duy,
                                 ctx->ABCDx, ctx->ABCDy,
                                 ix - ix0, iy - iy0,

                                 // control points
                                 &intrinsics[*ivar0],
                                 2*config->Nx);

    pt_out[i_pt].x = ctx->u.x * fxy.x + config->cx;
    pt_out[i_pt].y = ctx->u.y * fxy.y + config->cy;


    // u = stereographic(p)
    // q = [ ux * fx(u, intrinsics) + cx ]
    //   = [ uy * fy(u, intrinsics) + cy ]
    //
    // Extrinsics:
    //   dqx/deee = ux dfx/duxy duxy/deee + fx dux/deee
    //            = ux (dfx/dux dux/deee + dfx/duy duy/deee) + fx dux/deee
    //            = (ux dfx/dux + fx) dux/deee + ux dfx/duy duy/deee
    //   dqy/deee = (uy dfy/duy + fy) duy/deee + uy dfy/dux dux/deee
    void propagate_extrinsics( point3_t* dxy_deee,
                               const double* dptcam_deee)
    {
        double du_deee[2][3];
        double s0,s1;
        mul_genN3_gen33_vout(2, (double*)du_dp, dptcam_deee, (double*)du_deee);
        s0 = ctx->u.x*dfxy_dux[0] + fxy.x;
        s1 = ctx->u.x*dfxy_duy[0];
        for(int i=0; i<3; i++)
            dxy_deee[i_pt*2+0].xyz[i] = s0*du_deee[0][i] + s1*du_deee[1][i];
        s0 = ctx->u.y*dfxy_duy[1] + fxy.y;
        s1 = ctx->u.y*dfxy_dux[1];
        for(int i=0; i<3; i++)
            dxy_deee[i_pt*2+1].xyz[i] = s0*du_deee[1][i] + s1*du_deee[0][i];
    }
    void propagate_extrinsics_cam0( point3_t* dxy_deee)
    {
        double s0,s1;
        s0 = ctx->u.x*dfxy_dux[0] + fxy.x;
        s1 = ctx->u.x*dfxy_duy[0];
        for(int i=0; i<3; i++)
            dxy_deee[i_pt*2+0].xyz[i] = s0*du_dp[0][i] + s1*du_dp[1][i];
        s0 = ctx->u.y*dfxy_duy[1] + fxy.y;
        s1 = ctx->u.y*dfxy_dux[1];
        for(int i=0; i<3; i++)
            dxy_deee[i_pt*2+1].xyz[i] = s0*du_dp[1][i] + s1*du_dp[0][i];
    }
    if(camera_at_identity)
    {
        if( dxy_drcamera != NULL ) memset(dxy_drcamera[2*i_pt].xyz, 0, 6*sizeof(double));
        if( dxy_dtcamera != NULL ) memset(dxy_dtcamera[2*i_pt].xyz, 0, 6*sizeof(double));
        if( dxy_drframe  != NULL ) propagate_extrinsics( dxy_drframe,  dptcam_drf );
        if( dxy_dtframe  != NULL ) propagate_extrinsics_cam0( dxy_dtframe );
    }
    else
    {
        if( dxy_drcamera != NULL ) propagate_extrinsics( dxy_drcamera, dptcam_drc );
        if( dxy_dtcamera != NULL ) propagate_extrinsics( dxy_dtcamera, dptcam_dtc );
        if( dxy_drframe  != NULL ) propagate_extrinsics( dxy_drframe,  dptcam_drf );
        if( dxy_dtframe  != NULL ) propagate_extrinsics( dxy_dtframe,  dptcam_dtf );
    }
}

typedef struct
{
    uint16_t run_side_length;
    uint16_t ivar_stridey;
} gradient_sparse_meta_t;

// Projects 3D point(s), and reports the projection, and all the gradients. This
// is the main internal callback in the optimizer. This operates in one of two modes:
//
// if(calibration_object_width_n == 0) then we're projecting ONE point. In world
// coords this point is at frame_rt->t. frame_rt->r is not referenced. pt_out and the
// gradients reference 2 values (x,y in the imager)
//
// if(calibration_object_width_n > 0) then we're projecting a whole calibration
// object. The pose of this object is given in frame_rt. We project ALL
// calibration_object_width_n*calibration_object_width_n points. pt_out and the
// gradients reference ALL of these points
static
void project( // out
             point2_t* restrict pt_out,

             // The intrinsics gradients. These are split among several arrays.
             // High-parameter-count lens models can return their gradients
             // sparsely. All the actual gradient values live in
             // dxy_dintrinsics_pool_double, a buffer supplied by the caller. If
             // dxy_dintrinsics_pool_double is not NULL, the rest of the
             // variables are assumed non-NULL, and we compute all the
             // intrinsics gradients. Conversely, if dxy_dintrinsics_pool_double
             // is NULL, no intrinsics gradients are computed
             double*  restrict dxy_dintrinsics_pool_double,
             int*     restrict dxy_dintrinsics_pool_int,
             double** restrict dxy_dfxy,
             double** restrict dxy_dintrinsics_nocore,
             gradient_sparse_meta_t* gradient_sparse_meta,

             point3_t* restrict dxy_drcamera,
             point3_t* restrict dxy_dtcamera,
             point3_t* restrict dxy_drframe,
             point3_t* restrict dxy_dtframe,
             point2_t* restrict dxy_dcalobject_warp,

             // in

             // everything; includes the core, if there is one
             const double* restrict intrinsics,
             const pose_t* restrict camera_rt,
             const pose_t* restrict frame_rt,
             const point2_t* restrict calobject_warp,

             bool camera_at_identity, // if true, camera_rt is unused
             lensmodel_t lensmodel,

             // point index. If <0, a point at frame_rt->t is
             // assumed; frame_rt->r isn't referenced, and
             // dxy_drframe is expected to be NULL. And the
             // calibration_object_... variables aren't used either

             double calibration_object_spacing,
             int    calibration_object_width_n)
{
    const int Npoints =
        calibration_object_width_n ?
        calibration_object_width_n*calibration_object_width_n : 1;

    // I need to compose two transformations
    //
    // (object in reference frame) -> [frame transform] -> (object in camera0 frame) ->
    // -> [camera rt] -> (camera frame)
    //
    // Note that here the frame transform transforms TO the camera0 frame and
    // the camera transform transforms FROM the camera0 frame. This is how my
    // data is expected to be set up
    //
    // [Rc tc] [Rf tf] = [Rc*Rf  Rc*tf + tc]
    // [0  1 ] [0  1 ]   [0      1         ]
    //
    // This transformation (and its gradients) is handled by cvComposeRT() I
    // refer to the camera*frame transform as the "joint" transform, or the
    // letter j
    geometric_gradients_t gg;

    CvMat d_rj_rf = cvMat(3,3, CV_64FC1, gg._d_rj_rf);
    CvMat d_rj_tf = cvMat(3,3, CV_64FC1, gg._d_rj_tf);
    CvMat d_rj_rc = cvMat(3,3, CV_64FC1, gg._d_rj_rc);
    CvMat d_rj_tc = cvMat(3,3, CV_64FC1, gg._d_rj_tc);
    CvMat d_tj_rf = cvMat(3,3, CV_64FC1, gg._d_tj_rf);
    CvMat d_tj_tf = cvMat(3,3, CV_64FC1, gg._d_tj_tf);
    CvMat d_tj_rc = cvMat(3,3, CV_64FC1, gg._d_tj_rc);
    CvMat d_tj_tc = cvMat(3,3, CV_64FC1, gg._d_tj_tc);

    double _rj[3];
    CvMat  rj = cvMat(3,1,CV_64FC1, _rj);
    double _tj[3];
    CvMat  tj = cvMat(3,1,CV_64FC1, _tj);
    CvMat* p_rj;
    CvMat* p_tj;

    const double zero3[3] = {};
    // removing const, but that's just because OpenCV's API is incomplete. It IS
    // const
    CvMat rf = cvMat(3,1, CV_64FC1, (double*)(calibration_object_width_n ? frame_rt->r.xyz : zero3));
    CvMat tf = cvMat(3,1, CV_64FC1, (double*)frame_rt->t.xyz);

    if(!camera_at_identity)
    {
        // removing const here, but that's just because OpenCV's API is
        // incomplete. It IS const
        CvMat rc = cvMat(3,1, CV_64FC1, (double*)camera_rt->r.xyz);
        CvMat tc = cvMat(3,1, CV_64FC1, (double*)camera_rt->t.xyz);

        cvComposeRT( &rf,      &tf,
                     &rc,      &tc,
                     &rj,      &tj,
                     &d_rj_rf, &d_rj_tf,
                     &d_rj_rc, &d_rj_tc,
                     &d_tj_rf, &d_tj_tf,
                     &d_tj_rc, &d_tj_tc );
        p_rj = &rj;
        p_tj = &tj;
    }
    else
    {
        // We're looking at camera0, so this camera transform is fixed at the
        // identity. We don't need to compose anything, nor propagate gradients
        // for the camera extrinsics, since those don't exist in the parameter
        // vector

        // Here the "joint" transform is just the "frame" transform
        p_rj = &rf;
        p_tj = &tf;
    }

    if( LENSMODEL_IS_OPENCV(lensmodel.type) )
    {
        // Special-case path for opencv. This isn't strictly required, but
        // opencv has some optimized functions for this, and they should be a
        // bit faster than what I'm doing
        double* p_dxy_dfxy               = NULL;
        double* p_dxy_dintrinsics_nocore = NULL;
        if(dxy_dintrinsics_pool_double != NULL)
        {
            *dxy_dfxy                             = &dxy_dintrinsics_pool_double[0];
            *dxy_dintrinsics_nocore               = &dxy_dintrinsics_pool_double[Npoints*2];
            gradient_sparse_meta->run_side_length = 0;

            p_dxy_dfxy               = *dxy_dfxy;
            p_dxy_dintrinsics_nocore = *dxy_dintrinsics_nocore;
        }

        project_opencv( pt_out,
                        p_dxy_dfxy, p_dxy_dintrinsics_nocore,

                        dxy_drcamera,
                        dxy_dtcamera,
                        dxy_drframe,
                        dxy_dtframe,
                        dxy_dcalobject_warp,

                        intrinsics, calobject_warp,

                        calibration_object_spacing,
                        calibration_object_width_n,

                        mrcal_getNlensParams(lensmodel)-4,
                        camera_at_identity ? NULL : &gg,
                        p_rj, p_tj);
        return;
    }


    // Not using OpenCV distortions, the distortion and projection are not
    // coupled
    double _Rj[3*3];
    CvMat  Rj = cvMat(3,3,CV_64FC1, _Rj);
    double _d_Rj_rj[9*3];
    CvMat d_Rj_rj = cvMat(9,3,CV_64F, _d_Rj_rj);

    cvRodrigues2(p_rj, &Rj, &d_Rj_rj);


    double* p_dxy_dfxy               = NULL;
    double* p_dxy_dintrinsics_nocore = NULL;
    bool hascore = mrcal_modelHasCore_fxfycxcy(lensmodel);
    if(dxy_dintrinsics_pool_double != NULL)
    {
        if(hascore)
        {
            *dxy_dfxy                             = &dxy_dintrinsics_pool_double[0];
            *dxy_dintrinsics_nocore               = &dxy_dintrinsics_pool_double[Npoints*2];
            gradient_sparse_meta->run_side_length = 0;

            p_dxy_dfxy               = *dxy_dfxy;
            p_dxy_dintrinsics_nocore = *dxy_dintrinsics_nocore;
        }
        else
        {
            *dxy_dfxy               = NULL;
            *dxy_dintrinsics_nocore = NULL;

            if(lensmodel.type != LENSMODEL_SPLINED_STEREOGRAPHIC)
            {
                MSG("Unhandled lens model: %d (%s)",
                    lensmodel.type,
                    mrcal_lensmodel_name(lensmodel));
                assert(0);
            }
            const LENSMODEL_SPLINED_STEREOGRAPHIC__config_t* config =
                &lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config;
            *gradient_sparse_meta =
                (gradient_sparse_meta_t)
                {
                    .run_side_length = 4,
                    .ivar_stridey    = 2*config->Nx
                };
        }
    }

    // These are produced by propagate_extrinsics() and consumed by
    // project_point()
    double _dptcam_drc[3*3];
    double _dptcam_dtc[3*3];
    double _dptcam_drf[3*3];
    double _dptcam_dtf[3*3];
    double* dptcam_drc;
    double* dptcam_dtc;
    double* dptcam_drf;
    double* dptcam_dtf;

    point3_t propagate_extrinsics( const point3_t* pt_ref,
                                   const geometric_gradients_t* gg,
                                   const double* _Rj, const double* _d_Rj_rj,
                                   const double* _tj )
    {
        // I have
        //   p = Rj pref + tj
        //
        // Then parametric and non-parametric models do different things:
        //
        // parametric models:
        //   u = distort(p, distortions)
        //   q = uxy/uz * fxy + cxy
        //
        //   extrinsic gradients:
        //       dqx/deee = d( ux/uz * fx + cx)/deee =
        //                = fx d(ux/uz)/deee =
        //                = fx/uz^2 ( uz dux/deee - duz/deee ux )
        //
        // nonparametric (splined) models
        //   uxy = stereographic(p)
        //   q   = [ ux * fx(uxy, intrinsics) + cx ]
        //       = [ uy * fy(uxy, intrinsics) + cy ]
        //
        //   Extrinsics:
        //     dqx/deee = ux dfx/duxy duxy/deee + fx dux/deee
        //              = ux (dfx/dux dux/deee + dfx/duy duy/deee) + fx dux/deee
        //              = (ux dfx/dux + fx I) dux/deee + ux dfx/duy duy/deee
        //     dqy/deee = (uy dfy/duy + fy I) duy/deee + uy dfy/dux dux/deee
        //
        //   Intrinsics:
        //     dqx/diii = ux dfx/diii
        //
        // So the two kinds of models have completely different expressions for
        // their gradients, and I implement them separately

        // Rj * pt + tj -> pt
        point3_t pt_cam;
        mul_vec3_gen33t_vout(pt_ref->xyz, _Rj, pt_cam.xyz);
        add_vec(3, pt_cam.xyz,  _tj);

        void propagate_extrinsics_one(double* dptcam_dparam,
                                      const double* drj_dparam,
                                      const double* dtj_dparam,
                                      const double* _d_Rj_rj)
        {
            // dRj[row0]/drj is 3x3 matrix at &_d_Rj_rj[0]
            // dRj[row0]/drc = dRj[row0]/drj * drj_drc
            for(int i=0; i<3; i++)
            {
                mul_vec3_gen33_vout( pt_ref->xyz, &_d_Rj_rj[9*i], &dptcam_dparam[3*i] );
                mul_vec3_gen33     ( &dptcam_dparam[3*i],   drj_dparam);
                add_vec(3, &dptcam_dparam[3*i], &dtj_dparam[3*i] );
            }
        }
        void propagate_extrinsics_one_cam0(double* dptcam_rf,
                                           const double* _d_Rf_rf)
        {
            // dRj[row0]/drj is 3x3 matrix at &_d_Rf_rf[0]
            // dRj[row0]/drc = dRj[row0]/drj * drj_drc
            for(int i=0; i<3; i++)
                mul_vec3_gen33_vout( pt_ref->xyz, &_d_Rf_rf[9*i], &dptcam_rf[3*i] );
        }
        if(gg != NULL)
        {
            propagate_extrinsics_one(_dptcam_drc, gg->_d_rj_rc, gg->_d_tj_rc, _d_Rj_rj);
            propagate_extrinsics_one(_dptcam_dtc, gg->_d_rj_tc, gg->_d_tj_tc, _d_Rj_rj);
            propagate_extrinsics_one(_dptcam_drf, gg->_d_rj_rf, gg->_d_tj_rf, _d_Rj_rj);
            propagate_extrinsics_one(_dptcam_dtf, gg->_d_rj_tf, gg->_d_tj_tf, _d_Rj_rj);
            dptcam_drc = _dptcam_drc;
            dptcam_dtc = _dptcam_dtc;
            dptcam_drf = _dptcam_drf;
            dptcam_dtf = _dptcam_dtf;
        }
        else
        {
            // camera is at the reference. The "joint" coord system is the "frame"
            // coord system
            //
            //   p_cam = Rf p_ref + tf
            //
            // dp/drc = 0
            // dp/dtc = 0
            // dp/drf = reshape(dRf_drf p_ref)
            // dp/dtf = I
            propagate_extrinsics_one_cam0(_dptcam_drf, _d_Rj_rj);

            dptcam_drc = NULL;
            dptcam_dtc = NULL;
            dptcam_drf = _dptcam_drf;
            dptcam_dtf = NULL; // this is I. The user of this MUST know to interpret
            // it that way
        }
        return pt_cam;
    }

    void project_point( // outputs
                       point2_t* pt_out,
                       double* p_dxy_dfxy, double* p_dxy_dintrinsics_nocore,
                       point3_t* restrict dxy_drcamera,
                       point3_t* restrict dxy_dtcamera,
                       point3_t* restrict dxy_drframe,
                       point3_t* restrict dxy_dtframe,
                       point2_t* restrict dxy_dcalobject_warp,
                       // inputs
                       const point3_t* pt_cam,
                       const double* restrict intrinsics,
                       lensmodel_t lensmodel,
                       const point2_t* dpt_ref2_dwarp, int i_pt,
                       // if NULL then the camera is at the reference
                       bool camera_at_identity,
                       const double* _Rj)
    {
        if(!hascore)
        {
            splined_intrinsics_grad_context_t ctx;
            int ivar0;
            _project_point_splined( // outputs
                                   pt_out,
                                   &ctx, &ivar0,

                                   dxy_drcamera,
                                   dxy_dtcamera,
                                   dxy_drframe,
                                   dxy_dtframe,
                                   dxy_dcalobject_warp,

                                   // inputs
                                   pt_cam,
                                   dptcam_drc, dptcam_dtc, dptcam_drf, dptcam_dtf,
                                   intrinsics,
                                   camera_at_identity,
                                   lensmodel,
                                   dpt_ref2_dwarp, i_pt,
                                   _Rj);
            if(dxy_dintrinsics_pool_int != NULL)
            {
                *(dxy_dintrinsics_pool_int++) = ivar0;
                memcpy(dxy_dintrinsics_pool_double, &ctx,
                       sizeof(ctx));
                dxy_dintrinsics_pool_double = &dxy_dintrinsics_pool_double[sizeof(ctx)/sizeof(double)];
            }
        }
        else
        {
            _project_point_parametric( // outputs
                                      pt_out,
                                      p_dxy_dfxy, p_dxy_dintrinsics_nocore,
                                      dxy_drcamera,
                                      dxy_dtcamera,
                                      dxy_drframe,
                                      dxy_dtframe,
                                      dxy_dcalobject_warp,

                                      // inputs
                                      pt_cam,
                                      dptcam_drc, dptcam_dtc, dptcam_drf, dptcam_dtf,
                                      intrinsics,
                                      camera_at_identity,
                                      lensmodel,
                                      dpt_ref2_dwarp, i_pt,
                                      _Rj);
        }
    }





    if( !calibration_object_width_n )
    {
        point3_t pt_cam =
            propagate_extrinsics( &(point3_t){},
                                  camera_at_identity ? NULL : &gg,
                                  _Rj, _d_Rj_rj, p_tj->data.db);
        project_point(  pt_out,
                        p_dxy_dfxy, p_dxy_dintrinsics_nocore,
                        dxy_drcamera, dxy_dtcamera, dxy_drframe, dxy_dtframe, dxy_dcalobject_warp,

                        &pt_cam,
                        intrinsics, lensmodel,
                        NULL, 0,
                        camera_at_identity, _Rj);
    }
    else
    {
        int i_pt = 0;

         // The calibration object has a simple grid geometry
        for(int y = 0; y<calibration_object_width_n; y++)
            for(int x = 0; x<calibration_object_width_n; x++)
            {
                point3_t pt_ref = {.x = (double)x * calibration_object_spacing,
                                   .y = (double)y * calibration_object_spacing};
                point2_t dpt_ref2_dwarp = {};

                if(calobject_warp != NULL)
                {
                    // Add a board warp here. I have two parameters, and they describe
                    // additive flex along the x axis and along the y axis, in that
                    // order. In each direction the flex is a parabola, with the
                    // parameter k describing the max deflection at the center. If the
                    // ends are at +- 1 I have d = k*(1 - x^2). If the ends are at
                    // (0,N-1) the equivalent expression is: d = k*( 1 - 4*x^2/(N-1)^2 +
                    // 4*x/(N-1) - 1 ) = d = 4*k*(x/(N-1) - x^2/(N-1)^2) = d =
                    // 4.*k*x*r(1. - x*r)
                    double r = 1./(double)(calibration_object_width_n-1);

                    double xr = (double)x * r;
                    double yr = (double)y * r;
                    double dx = 4. * xr * (1. - xr);
                    double dy = 4. * yr * (1. - yr);
                    pt_ref.z += calobject_warp->x * dx;
                    pt_ref.z += calobject_warp->y * dy;
                    dpt_ref2_dwarp.x = dx;
                    dpt_ref2_dwarp.y = dy;
                }

                point3_t pt_cam =
                    propagate_extrinsics( &pt_ref,
                                          camera_at_identity ? NULL : &gg,
                                          _Rj, _d_Rj_rj, p_tj->data.db);
                project_point(pt_out,
                              p_dxy_dfxy, p_dxy_dintrinsics_nocore,
                              dxy_drcamera, dxy_dtcamera, dxy_drframe, dxy_dtframe, dxy_dcalobject_warp,

                              &pt_cam,
                              intrinsics, lensmodel,
                              &dpt_ref2_dwarp, i_pt,
                              camera_at_identity, _Rj);
                i_pt++;
            }
    }
}

// Compute the region-of-interest weight. The region I care about is in r=[0,1];
// here the weight is ~ 1. Past that, the weight falls off. I don't attenuate
// all the way to 0 to preserve the constraints of the problem. Letting these go
// to 0 could make the problem indeterminate
static double region_of_interest_weight_from_unitless_rad(double rsq)
{
    if( rsq < 1.0 ) return 1.0;
    return 1e-3;
}
static double region_of_interest_weight(const point3_t* pt,
                                        const double* roi, int i_camera)
{
    if(roi == NULL) return 1.0;

    roi = &roi[4*i_camera];
    double dx = (pt->x - roi[0]) / roi[2];
    double dy = (pt->y - roi[1]) / roi[3];

    return region_of_interest_weight_from_unitless_rad(dx*dx + dy*dy);
}

static
bool _project_cahvore( // out
                      point2_t* out,

                      // in
                      const point3_t* v,
                      int N,

                      // core, distortions concatenated
                      const double* intrinsics)
{
    // Apply a CAHVORE warp to an un-distorted point

    //  Given intrinsic parameters of a CAHVORE model and a set of
    //  camera-coordinate points, return the projected point(s)

    // This comes from cmod_cahvore_3d_to_2d_general() in
    // m-jplv/libcmod/cmod_cahvore.c
    //
    // The lack of documentation here comes directly from the lack of
    // documentation in that function.

    // I parametrize the optical axis such that
    // - o(alpha=0, beta=0) = (0,0,1) i.e. the optical axis is at the center
    //   if both parameters are 0
    // - The gradients are cartesian. I.e. do/dalpha and do/dbeta are both
    //   NOT 0 at (alpha=0,beta=0). This would happen at the poles (gimbal
    //   lock), and that would make my solver unhappy
    // So o = { s_al*c_be, s_be,  c_al*c_be }
    const intrinsics_core_t* core = (const intrinsics_core_t*)intrinsics;
    const double alpha     = intrinsics[4 + 0];
    const double beta      = intrinsics[4 + 1];
    const double r0        = intrinsics[4 + 2];
    const double r1        = intrinsics[4 + 3];
    const double r2        = intrinsics[4 + 4];
    const double e0        = intrinsics[4 + 5];
    const double e1        = intrinsics[4 + 6];
    const double e2        = intrinsics[4 + 7];
    const double linearity = intrinsics[4 + 8];

    double sa,ca;
    sincos(alpha, &sa, &ca);
    double sb,cb;
    sincos(beta, &sb, &cb);

    const double o[] ={ cb * sa, sb, cb * ca };

    for(int i_pt=0; i_pt<N; i_pt++)
    {
        ///////////////// THIS IS MADE UP, AND PROBABLY WRONG I'm using jplv as
        // the reference implementation for this, but that implementation can't
        // work. In jplv project(v) and project(k*v) don't project to the sample
        // point. Look at the definition of upsilon below. omega and l have
        // units of m, while the other terms are unitless. I'm hypothesizing
        // that they meant to normalize v, but never did it. So I'm going that
        // here. mrcal supports cahvore only for compatibility, so nobody's
        // using this code. IF YOU ARE GOING TO USE THIS CODE, PLEASE CONFIRM
        // THAT THIS CAHVORE PROJECTION IS CORRECT
        double vhere[3] = {
            v[i_pt].x,
            v[i_pt].y,
            v[i_pt].z
        };
        double vnorm = sqrt(vhere[0]*vhere[0] +
                            vhere[1]*vhere[1] +
                            vhere[2]*vhere[2]);
        for(int i=0; i<3; i++) vhere[i] /= vnorm;

        // cos( angle between v and o ) = inner(v,o) / (norm(o) * norm(v)) =
        // omega/norm(v)
        double omega = vhere[0]*o[0] + vhere[1]*o[1] + vhere[2]*o[2];


        // Basic Computations

        // Calculate initial terms
        double u[3];
        for(int i=0; i<3; i++) u[i] = omega*o[i];

        double ll[3];
        for(int i=0; i<3; i++) ll[i] = vhere[i]-u[i];
        double l = sqrt(ll[0]*ll[0] + ll[1]*ll[1] + ll[2]*ll[2]);

        // Calculate theta using Newton's Method
        double theta = atan2(l, omega);

        int inewton;
        for( inewton = 100; inewton; inewton--)
        {
            // Compute terms from the current value of theta
            double sth,cth;
            sincos(theta, &sth, &cth);

            double theta2  = theta * theta;
            double theta3  = theta * theta2;
            double theta4  = theta * theta3;
            double upsilon =
                omega*cth + l*sth
                - (1.0   - cth) * (e0 +      e1*theta2 +     e2*theta4)
                - (theta - sth) * (      2.0*e1*theta  + 4.0*e2*theta3);

            // Update theta
            double dtheta =
                (
                 omega*sth - l*cth
                 - (theta - sth) * (e0 + e1*theta2 + e2*theta4)
                 ) / upsilon;

            theta -= dtheta;

            // Check exit criterion from last update
            if(fabs(dtheta) < 1e-8)
                break;
        }
        if(inewton == 0)
        {
            fprintf(stderr, "%s(): too many iterations\n", __func__);
            return false;
        }

        // got a theta

        // Check the value of theta
        if(theta * fabs(linearity) > M_PI/2.)
        {
            fprintf(stderr, "%s(): theta out of bounds\n", __func__);
            return false;
        }

        // If we aren't close enough to use the small-angle approximation ...
        if (theta > 1e-8)
        {
            // ... do more math!
            double linth = linearity * theta;
            double chi;
            if (linearity < -1e-15)
                chi = sin(linth) / linearity;
            else if (linearity > 1e-15)
                chi = tan(linth) / linearity;
            else
                chi = theta;

            double chi2 = chi * chi;
            double chi3 = chi * chi2;
            double chi4 = chi * chi3;

            double zetap = l / chi;

            double mu = r0 + r1*chi2 + r2*chi4;

            double uu[3];
            for(int i=0; i<3; i++) uu[i] = zetap*o[i];
            double vv[3];
            for(int i=0; i<3; i++) vv[i] = (1. + mu)*ll[i];

            for(int i=0; i<3; i++)
                u[i] = uu[i] + vv[i];
            // now I apply a normal projection to the warped 3d point v
            out[i_pt].x = core->focal_xy[0] * u[0]/u[2] + core->center_xy[0];
            out[i_pt].y = core->focal_xy[1] * u[1]/u[2] + core->center_xy[1];
        }
        else
        {
            // now I apply a normal projection to the warped 3d point v
            out[i_pt].x = core->focal_xy[0] * vhere[0]/vhere[2] + core->center_xy[0];
            out[i_pt].y = core->focal_xy[1] * vhere[1]/vhere[2] + core->center_xy[1];
        }
    }
    return true;
}

// Wrapper around the internal project() function. This is the function used in
// the inner optimization loop to map world points to their observed pixel
// coordinates, and to optionally provide gradients. dq_dintrinsics and/or
// dq_dp are allowed to be NULL if we're not interested in gradients.
//
// This function supports CAHVORE distortions if we don't ask for gradients
bool mrcal_project( // out
                   point2_t* q,

                   // core, distortions concatenated. Stored as a row-first
                   // array of shape (N,2,Nintrinsics). This is a DENSE array.
                   // High-parameter-count lens models have very sparse
                   // gradients here, and the internal project() function
                   // returns those sparsely. For now THIS function densifies
                   // all of these
                   double*   dq_dintrinsics,
                   // Stored as a row-first array of shape (N,2,3). Each
                   // trailing ,3 dimension element is a point3_t
                   point3_t* dq_dp,

                   // in
                   const point3_t* p,
                   int N,
                   lensmodel_t lensmodel,
                   // core, distortions concatenated
                   const double* intrinsics)
{
    // project() doesn't handle cahvore, so I special-case it here
    if( lensmodel.type == LENSMODEL_CAHVORE )
    {
        if(dq_dintrinsics != NULL || dq_dp != NULL)
        {
            fprintf(stderr, "mrcal_project(LENSMODEL_CAHVORE) is not yet implemented if we're asking for gradients\n");
            return false;
        }
        return _project_cahvore(q, p, N, intrinsics);
    }

    int Nintrinsics  = mrcal_getNlensParams(lensmodel);

    // Special-case for opencv/pinhole and projection-only. cvProjectPoints2 and
    // project() have a lot of overhead apparently, and calling either in a loop
    // is very slow. I can call it once, and use its fast internal loop,
    // however. This special case does the same thing, but much faster.
    if(dq_dintrinsics == NULL && dq_dp == NULL &&
       (LENSMODEL_IS_OPENCV(lensmodel.type) ||
        lensmodel.type == LENSMODEL_PINHOLE))
    {
        int Ndistortions = Nintrinsics - 4; // ignoring fx,fy,cx,cy

        const intrinsics_core_t* intrinsics_core = (const intrinsics_core_t*)intrinsics;
        double fx = intrinsics_core->focal_xy [0];
        double fy = intrinsics_core->focal_xy [1];
        double cx = intrinsics_core->center_xy[0];
        double cy = intrinsics_core->center_xy[1];
        double _camera_matrix[] =
            { fx,  0, cx,
              0,  fy, cy,
              0,   0,  1 };
        CvMat camera_matrix = cvMat(3,3, CV_64FC1, _camera_matrix);

        CvMat object_points  = cvMat(3,N, CV_64FC1, (double*)p->xyz);
        CvMat image_points   = cvMat(2,N, CV_64FC1, (double*)q->xy);
        double _zero3[3] = {};
        CvMat zero3 = cvMat(3,1,CV_64FC1, _zero3);

        if(Ndistortions > 0)
        {
            CvMat _distortions =
                cvMat( Ndistortions, 1, CV_64FC1,
                       // removing const, but that's just because
                       // OpenCV's API is incomplete. It IS const
                       (double*)&intrinsics[4]);
            cvProjectPoints2(&object_points,
                             &zero3, &zero3,
                             &camera_matrix,
                             &_distortions,
                             &image_points,
                             NULL, NULL, NULL, NULL, NULL, 0 );
        }
        else
            cvProjectPoints2(&object_points,
                             &zero3, &zero3,
                             &camera_matrix,
                             NULL,
                             &image_points,
                             NULL, NULL, NULL, NULL, NULL, 0 );
        return true;
    }


    // Some models have sparse gradients, but I'm returning a dense array here.
    // So I init everything at 0
    if(dq_dintrinsics != NULL)
        memset(dq_dintrinsics, 0, N*2*Nintrinsics*sizeof(double));

    for(int i=0; i<N; i++)
    {
        pose_t frame = {.r = {},
                        .t = p[i]};

        // simple non-intrinsics-gradient path. dp_dp is handled entirely in
        // project()
        if( dq_dintrinsics == NULL )
            project( q,
                     NULL, NULL, NULL, NULL, NULL,
                     NULL, NULL, NULL, dq_dp, NULL,

                     // in
                     intrinsics, NULL, &frame, NULL, true, lensmodel, 0.0, 0);
        else
        {
            double dxy_dintrinsics_pool_double[2*(1+Nintrinsics-4)];
            int    dxy_dintrinsics_pool_int   [1];
            double* dxy_dfxy               = NULL;
            double* dxy_dintrinsics_nocore = NULL;
            gradient_sparse_meta_t gradient_sparse_meta = {}; // init to pacify compiler warning

            project( q,

                     dxy_dintrinsics_pool_double,
                     dxy_dintrinsics_pool_int,
                     &dxy_dfxy, &dxy_dintrinsics_nocore, &gradient_sparse_meta,

                     NULL, NULL, NULL, dq_dp, NULL,

                     // in
                     intrinsics, NULL, &frame, NULL, true, lensmodel, 0.0, 0);

            int Ncore = 0;
            if(dxy_dfxy != NULL)
            {
                Ncore = 4;

                // fxy. off-diagonal elements are 0
                dq_dintrinsics[0*Nintrinsics + 0] = dxy_dfxy[0];
                dq_dintrinsics[0*Nintrinsics + 1] = 0.0;
                dq_dintrinsics[1*Nintrinsics + 0] = 0.0;
                dq_dintrinsics[1*Nintrinsics + 1] = dxy_dfxy[1];

                // cxy. Identity
                dq_dintrinsics[0*Nintrinsics + 2] = 1.0;
                dq_dintrinsics[0*Nintrinsics + 3] = 0.0;
                dq_dintrinsics[1*Nintrinsics + 2] = 0.0;
                dq_dintrinsics[1*Nintrinsics + 3] = 1.0;
            }
            if( dxy_dintrinsics_nocore != NULL )
            {
                for(int i_xy=0; i_xy<2; i_xy++)
                    memcpy(&dq_dintrinsics[i_xy*Nintrinsics + Ncore],
                           &dxy_dintrinsics_nocore[i_xy*(Nintrinsics-Ncore)],
                           (Nintrinsics-Ncore)*sizeof(double));
            }
            if(gradient_sparse_meta.run_side_length > 0)
            {
                // u = stereographic(p)
                // q = [ ux * fx(u, intrinsics) + cx ]
                //   = [ uy * fy(u, intrinsics) + cy ]
                //
                // Intrinsics:
                //   dqx/diii = ux dfx/diii
                //
                // dfx/diii = flatten(ABCDx[0..3] * ABCDy[0..3])

                // pool_double contains ABCDxy and u in a
                // splined_intrinsics_grad_context_t
                const splined_intrinsics_grad_context_t* ctx =
                    (splined_intrinsics_grad_context_t*)dxy_dintrinsics_pool_double;
                const int ivar0 = dxy_dintrinsics_pool_int[0];

                const int len          = gradient_sparse_meta.run_side_length;
                const int ivar_stridey = gradient_sparse_meta.ivar_stridey;

                for(int i_xy=0; i_xy<2; i_xy++)
                    for(int iy=0; iy<len; iy++)
                        for(int ix=0; ix<len; ix++)
                        {
                            dq_dintrinsics[ivar0 +
                                           ivar_stridey*iy +
                                           ix*2 +
                                           i_xy] =
                                ctx->ABCDx[ix]*ctx->ABCDy[iy]*ctx->u.xy[i_xy];
                        }
            }

            // advance
            dq_dintrinsics = &dq_dintrinsics[2*Nintrinsics];
            if(dq_dp != NULL)
                dq_dp = &dq_dp[2];
        }

        // advance to the next point
        q = &q[1];
    }
    return true;
}

bool mrcal_project_z1( // out
                       point2_t* q,

                       // core, distortions concatenated. Stored as a row-first
                       // array of shape (N,2,Nintrinsics)
                       double*   dq_dintrinsics,

                       // Stored as a row-first array of shape (N,2,2). Each
                       // trailing ,2 dimension element is a point2_t
                       point2_t* dq_dVxy,

                       // in
                       const point2_t* Vxy,
                       int N,
                       lensmodel_t lensmodel,
                       // core, distortions concatenated
                       const double* intrinsics)
{
    // I allocate/deallocate some temporary arrays, and use the normal
    // mrcal_project(). Would be nice to not allocate or deallocate anything
    int N_to_allocate = N;
    if(dq_dVxy != NULL)
        N_to_allocate += N*2;

    point3_t* pool = (point3_t*)malloc(N_to_allocate*sizeof(point3_t));
    if(!pool)
    {
        MSG("Couldn't not allocate memory for %d point3_t objects!",
            N_to_allocate);
        return false;
    }

    point3_t* p     = &pool[0];
    point3_t* dq_dp = dq_dVxy != NULL ? &pool[N] : NULL;

    for(int i=0; i<N; i++)
    {
        p[i].x = Vxy[i].x;
        p[i].y = Vxy[i].y;
        p[i].z = 1.0;
    }

    bool result = mrcal_project( q,
                                 dq_dintrinsics,
                                 dq_dp,
                                 p,
                                 N,
                                 lensmodel,
                                 intrinsics);
    if(!result)           goto done;
    if( dq_dVxy == NULL ) goto done;

    // I have dq/dp. I want dq/dVxy = dq/dp dp/dVxy
    // We constructed p from Vxy above, so dp/dVxy = [ I 0 ]t
    for(int i=0; i<N; i++)
    {
        dq_dVxy[2*i + 0].x = dq_dp[2*i + 0].x;
        dq_dVxy[2*i + 0].y = dq_dp[2*i + 0].y;
        dq_dVxy[2*i + 1].x = dq_dp[2*i + 1].x;
        dq_dVxy[2*i + 1].y = dq_dp[2*i + 1].y;
    }

 done:
    free(pool);

    return result;
}

// internal function for mrcal_unproject() and mrcal_unproject_z1()
static
bool _unproject( // out
                double* out,
                bool output_2d_z1,

                // in
                const point2_t* q,
                int N,
                lensmodel_t lensmodel,
                // core, distortions concatenated
                const double* intrinsics)
{
    if( lensmodel.type == LENSMODEL_CAHVORE )
    {
        fprintf(stderr, "mrcal_unproject(LENSMODEL_CAHVORE) not yet implemented. No gradients available\n");
        return false;
    }

    if(!mrcal_modelHasCore_fxfycxcy(lensmodel))
    {
        MSG("Can't unproject without a core yet. Need to change the 2d domain.");
        exit(1);
    }

    const intrinsics_core_t* core        = (const intrinsics_core_t*)intrinsics;
    const double*            distortions = &intrinsics[4];

    const double fx_recip_distort = 1.0 / core->focal_xy[0];
    const double fy_recip_distort = 1.0 / core->focal_xy[1];

    // easy special-case
    if( lensmodel.type == LENSMODEL_PINHOLE )
    {
        for(int i=0; i<N; i++)
        {
            out[0] = (q[i].x - core->center_xy[0]) * fx_recip_distort;
            out[1] = (q[i].y - core->center_xy[1]) * fy_recip_distort;
            if(output_2d_z1)
                out = &out[2];
            else
            {
                out[2] = 1.0;
                out = &out[3];
            }
        }
        return true;
    }

    pose_t frame = {.r = {},
                    .t = {.z = 1.0}};

    for(int i=0; i<N; i++)
    {
        void cb(const double*   xy,
                double*         x,
                double*         J,
                void*           cookie __attribute__((unused)))
        {
            // I want q[i] == project(xy1)
            frame.t.x = xy[0];
            frame.t.y = xy[1];
            // initializing this above: frame.t.z = 1.0;

            point3_t dxy_dtframe[2];
            point2_t q_hypothesis;
            project( &q_hypothesis,
                     NULL,NULL,NULL,NULL,NULL,
                     NULL, NULL, NULL, dxy_dtframe,
                     NULL,

                     // in
                     intrinsics,
                     NULL,
                     &frame,
                     NULL,
                     true,
                     lensmodel,
                     0.0, 0);
            x[0] = q_hypothesis.x - q[i].x;
            x[1] = q_hypothesis.y - q[i].y;
            J[0*2 + 0] = dxy_dtframe[0].x;
            J[0*2 + 1] = dxy_dtframe[0].y;
            J[1*2 + 0] = dxy_dtframe[1].x;
            J[1*2 + 1] = dxy_dtframe[1].y;
        }

        // seed from the distorted value
        out[0] = (q[i].x - core->center_xy[0]) * fx_recip_distort;
        out[1] = (q[i].y - core->center_xy[1]) * fy_recip_distort;


        dogleg_parameters2_t dogleg_parameters;
        dogleg_getDefaultParameters(&dogleg_parameters);
        dogleg_parameters.dogleg_debug = 0;
        double norm2x =
            dogleg_optimize_dense2(out, 2, 2, cb, NULL,
                                   &dogleg_parameters,
                                   NULL);
        //This needs to be precise; if it isn't, I barf. Shouldn't happen
        //very often

        static bool already_complained = false;
        if(norm2x/2.0 > 1e-4)
        {
            if(!already_complained)
            {
                MSG("WARNING: I wasn't able to precisely compute some points. norm2x=%f. Returning nan for those. Will complain just once",
                    norm2x);
                already_complained = true;
            }
            double nan = strtod("NAN", NULL);
            out[0] = nan;
            out[1] = nan;
        }
        if(output_2d_z1)
            out = &out[2];
        else
        {
            out[2] = 1.0;
            out = &out[3];
        }
    }
    return true;
}

// Maps a set of distorted 2D imager points q to a 3d vector in camera
// coordinates that produced these pixel observations. The 3d vector is defined
// up-to-length, so the vectors reported here will all have z = 1.
//
// This is the "reverse" direction, so an iterative nonlinear optimization is
// performed internally to compute this result. This is much slower than
// mrcal_project. For OpenCV distortions specifically, OpenCV has
// cvUndistortPoints() (and cv2.undistortPoints()), but these are inaccurate:
// https://github.com/opencv/opencv/issues/8811
//
// This function does NOT support CAHVORE
bool mrcal_unproject( // out
                     point3_t* out,

                     // in
                     const point2_t* q,
                     int N,
                     lensmodel_t lensmodel,
                     // core, distortions concatenated
                     const double* intrinsics)
{
    return _unproject(out->xyz, false, q,N,lensmodel,intrinsics);
}
// Exactly the same as mrcal_unproject(), but reports 2d points, omitting the
// redundant z=1
bool mrcal_unproject_z1( // out
                        point2_t* out,

                        // in
                        const point2_t* q,
                        int N,
                        lensmodel_t lensmodel,
                        // core, distortions concatenated
                        const double* intrinsics)
{
    return _unproject(out->xy, true, q,N,lensmodel,intrinsics);
}

// The following functions define/use the layout of the state vector. In general
// I do:
//
//   intrinsics_cam0
//   intrinsics_cam1
//   intrinsics_cam2
//   ...
//   extrinsics_cam1
//   extrinsics_cam2
//   extrinsics_cam3
//   ...
//   frame0
//   frame1
//   frame2
//   ....
//   calobject_warp0
//   calobject_warp1

// From real values to unit-scale values. Optimizer sees unit-scale values
static int pack_solver_state_intrinsics( // out
                                         double* p,

                                         // in
                                         const double* intrinsics,
                                         const lensmodel_t lensmodel,
                                         mrcal_problem_details_t problem_details,
                                         int Ncameras )
{
    int i_state = 0;

    int Nintrinsics  = mrcal_getNlensParams(lensmodel);
    int Ndistortions = Nintrinsics;
    if(mrcal_modelHasCore_fxfycxcy(lensmodel))
        Ndistortions -= 4; // ignoring fx,fy,cx,cy

    for(int i_camera=0; i_camera < Ncameras; i_camera++)
    {
        if( problem_details.do_optimize_intrinsic_core )
        {
            const intrinsics_core_t* intrinsics_core = (const intrinsics_core_t*)intrinsics;
            p[i_state++] = intrinsics_core->focal_xy [0] / SCALE_INTRINSICS_FOCAL_LENGTH;
            p[i_state++] = intrinsics_core->focal_xy [1] / SCALE_INTRINSICS_FOCAL_LENGTH;
            p[i_state++] = intrinsics_core->center_xy[0] / SCALE_INTRINSICS_CENTER_PIXEL;
            p[i_state++] = intrinsics_core->center_xy[1] / SCALE_INTRINSICS_CENTER_PIXEL;
        }

        if( problem_details.do_optimize_intrinsic_distortions )

            for(int i = 0; i<Ndistortions; i++)
                p[i_state++] = intrinsics[4 + i] / SCALE_DISTORTION;

        intrinsics = &intrinsics[Nintrinsics];
    }
    return i_state;
}
static void pack_solver_state( // out
                              double* p,

                              // in
                              const double* intrinsics, // Ncameras of these
                              const lensmodel_t lensmodel,
                              const pose_t*            extrinsics, // Ncameras-1 of these
                              const pose_t*            frames,     // Nframes of these
                              const point3_t*          points,     // Npoints of these
                              const point2_t*          calobject_warp, // 1 of these
                              mrcal_problem_details_t problem_details,
                              int Ncameras, int Nframes, int Npoints,

                              int Nstate_ref)
{
    int i_state = 0;

    i_state += pack_solver_state_intrinsics( p, intrinsics,
                                             lensmodel, problem_details,
                                             Ncameras );

    if( problem_details.do_optimize_extrinsics )
        for(int i_camera=1; i_camera < Ncameras; i_camera++)
        {
            p[i_state++] = extrinsics[i_camera-1].r.xyz[0] / SCALE_ROTATION_CAMERA;
            p[i_state++] = extrinsics[i_camera-1].r.xyz[1] / SCALE_ROTATION_CAMERA;
            p[i_state++] = extrinsics[i_camera-1].r.xyz[2] / SCALE_ROTATION_CAMERA;

            p[i_state++] = extrinsics[i_camera-1].t.xyz[0] / SCALE_TRANSLATION_CAMERA;
            p[i_state++] = extrinsics[i_camera-1].t.xyz[1] / SCALE_TRANSLATION_CAMERA;
            p[i_state++] = extrinsics[i_camera-1].t.xyz[2] / SCALE_TRANSLATION_CAMERA;
        }

    if( problem_details.do_optimize_frames )
    {
        for(int i_frame = 0; i_frame < Nframes; i_frame++)
        {
            p[i_state++] = frames[i_frame].r.xyz[0] / SCALE_ROTATION_FRAME;
            p[i_state++] = frames[i_frame].r.xyz[1] / SCALE_ROTATION_FRAME;
            p[i_state++] = frames[i_frame].r.xyz[2] / SCALE_ROTATION_FRAME;

            p[i_state++] = frames[i_frame].t.xyz[0] / SCALE_TRANSLATION_FRAME;
            p[i_state++] = frames[i_frame].t.xyz[1] / SCALE_TRANSLATION_FRAME;
            p[i_state++] = frames[i_frame].t.xyz[2] / SCALE_TRANSLATION_FRAME;
        }

        for(int i_point = 0; i_point < Npoints; i_point++)
        {
            p[i_state++] = points[i_point].xyz[0] / SCALE_POSITION_POINT;
            p[i_state++] = points[i_point].xyz[1] / SCALE_POSITION_POINT;
            p[i_state++] = points[i_point].xyz[2] / SCALE_POSITION_POINT;
        }
    }

    if( problem_details.do_optimize_calobject_warp )
    {
        p[i_state++] = calobject_warp->x / SCALE_CALOBJECT_WARP;
        p[i_state++] = calobject_warp->y / SCALE_CALOBJECT_WARP;
    }

    assert(i_state == Nstate_ref);
}

// Same as above, but packs/unpacks a vector instead of structures
void mrcal_pack_solver_state_vector( // out, in
                                     double* p, // unitless, FULL state on
                                                // input, scaled, decimated
                                                // (subject to problem_details),
                                                // meaningful state on output

                                     // in
                                     const lensmodel_t lensmodel,
                                     mrcal_problem_details_t problem_details,
                                     int Ncameras, int Nframes, int Npoints)
{
    int i_state = 0;

    i_state += pack_solver_state_intrinsics( p, p,
                                             lensmodel, problem_details,
                                             Ncameras );

    static_assert( offsetof(pose_t, r) == 0,
                   "pose_t has expected structure");
    static_assert( offsetof(pose_t, t) == 3*sizeof(double),
                   "pose_t has expected structure");
    if( problem_details.do_optimize_extrinsics )
        for(int i_camera=1; i_camera < Ncameras; i_camera++)
        {
            pose_t* extrinsics = (pose_t*)(&p[i_state]);

            p[i_state++] = extrinsics->r.xyz[0] / SCALE_ROTATION_CAMERA;
            p[i_state++] = extrinsics->r.xyz[1] / SCALE_ROTATION_CAMERA;
            p[i_state++] = extrinsics->r.xyz[2] / SCALE_ROTATION_CAMERA;

            p[i_state++] = extrinsics->t.xyz[0] / SCALE_TRANSLATION_CAMERA;
            p[i_state++] = extrinsics->t.xyz[1] / SCALE_TRANSLATION_CAMERA;
            p[i_state++] = extrinsics->t.xyz[2] / SCALE_TRANSLATION_CAMERA;
        }

    if( problem_details.do_optimize_frames )
    {
        for(int i_frame = 0; i_frame < Nframes; i_frame++)
        {
            pose_t* frames = (pose_t*)(&p[i_state]);
            p[i_state++] = frames->r.xyz[0] / SCALE_ROTATION_FRAME;
            p[i_state++] = frames->r.xyz[1] / SCALE_ROTATION_FRAME;
            p[i_state++] = frames->r.xyz[2] / SCALE_ROTATION_FRAME;

            p[i_state++] = frames->t.xyz[0] / SCALE_TRANSLATION_FRAME;
            p[i_state++] = frames->t.xyz[1] / SCALE_TRANSLATION_FRAME;
            p[i_state++] = frames->t.xyz[2] / SCALE_TRANSLATION_FRAME;
        }

        for(int i_point = 0; i_point < Npoints; i_point++)
        {
            point3_t* points = (point3_t*)(&p[i_state]);
            p[i_state++] = points->xyz[0] / SCALE_POSITION_POINT;
            p[i_state++] = points->xyz[1] / SCALE_POSITION_POINT;
            p[i_state++] = points->xyz[2] / SCALE_POSITION_POINT;
        }
    }

    if( problem_details.do_optimize_calobject_warp )
    {
        point2_t* calobject_warp = (point2_t*)(&p[i_state]);
        p[i_state++] = calobject_warp->x / SCALE_CALOBJECT_WARP;
        p[i_state++] = calobject_warp->y / SCALE_CALOBJECT_WARP;
    }
}

static int unpack_solver_state_intrinsics_onecamera( // out
                                                    intrinsics_core_t* intrinsics_core,
                                                    const lensmodel_t lensmodel,
                                                    double* distortions,

                                                    // in
                                                    const double* p,
                                                    int Nintrinsics,
                                                    mrcal_problem_details_t problem_details )
{
    int i_state = 0;
    if( problem_details.do_optimize_intrinsic_core )
    {
        intrinsics_core->focal_xy [0] = p[i_state++] * SCALE_INTRINSICS_FOCAL_LENGTH;
        intrinsics_core->focal_xy [1] = p[i_state++] * SCALE_INTRINSICS_FOCAL_LENGTH;
        intrinsics_core->center_xy[0] = p[i_state++] * SCALE_INTRINSICS_CENTER_PIXEL;
        intrinsics_core->center_xy[1] = p[i_state++] * SCALE_INTRINSICS_CENTER_PIXEL;
    }

    if( problem_details.do_optimize_intrinsic_distortions )
    {
        int Ncore = mrcal_modelHasCore_fxfycxcy(lensmodel) ? 4 : 0;
        for(int i = 0; i<Nintrinsics-Ncore; i++)
            distortions[i] = p[i_state++] * SCALE_DISTORTION;
    }

    return i_state;
}

static double get_scale_solver_state_intrinsics_onecamera( int i_state,
                                                           int Nintrinsics,
                                                           mrcal_problem_details_t problem_details )
{
    if( problem_details.do_optimize_intrinsic_core )
    {
        if( i_state < 4)
        {
            if( i_state < 2 ) return SCALE_INTRINSICS_FOCAL_LENGTH;
            return SCALE_INTRINSICS_CENTER_PIXEL;
        }

        i_state -= 4;
    }

    if( problem_details.do_optimize_intrinsic_distortions )
        if( i_state < Nintrinsics-4)
            return SCALE_DISTORTION;

    fprintf(stderr, "ERROR! %s() was asked about an out-of-bounds state\n", __func__);
    return -1.0;
}

static int unpack_solver_state_intrinsics( // out
                                           double* intrinsics, // Ncameras of
                                                               // these

                                           // in
                                           const double* p,
                                           const lensmodel_t lensmodel,
                                           mrcal_problem_details_t problem_details,
                                           int Ncameras )
{
    if( !problem_details.do_optimize_intrinsic_core &&
        !problem_details.do_optimize_intrinsic_distortions )
        return 0;


    int Nintrinsics = mrcal_getNlensParams(lensmodel);
    int i_state = 0;
    if(mrcal_modelHasCore_fxfycxcy(lensmodel))
        for(int i_camera=0; i_camera < Ncameras; i_camera++)
        {
            i_state +=
                unpack_solver_state_intrinsics_onecamera( (intrinsics_core_t*)intrinsics,
                                                          lensmodel,
                                                          &intrinsics[4],
                                                          &p[i_state], Nintrinsics, problem_details );
            intrinsics = &intrinsics[Nintrinsics];
        }
    else
        for(int i_camera=0; i_camera < Ncameras; i_camera++)
        {
            i_state +=
                unpack_solver_state_intrinsics_onecamera( NULL,
                                                          lensmodel,
                                                          intrinsics,
                                                          &p[i_state], Nintrinsics, problem_details );
            intrinsics = &intrinsics[Nintrinsics];
        }
    return i_state;
}

static int unpack_solver_state_extrinsics_one(// out
                                              pose_t* extrinsic,

                                              // in
                                              const double* p)
{
    int i_state = 0;
    extrinsic->r.xyz[0] = p[i_state++] * SCALE_ROTATION_CAMERA;
    extrinsic->r.xyz[1] = p[i_state++] * SCALE_ROTATION_CAMERA;
    extrinsic->r.xyz[2] = p[i_state++] * SCALE_ROTATION_CAMERA;

    extrinsic->t.xyz[0] = p[i_state++] * SCALE_TRANSLATION_CAMERA;
    extrinsic->t.xyz[1] = p[i_state++] * SCALE_TRANSLATION_CAMERA;
    extrinsic->t.xyz[2] = p[i_state++] * SCALE_TRANSLATION_CAMERA;
    return i_state;
}

static int unpack_solver_state_framert_one(// out
                                           pose_t* frame,

                                           // in
                                           const double* p)
{
    int i_state = 0;
    frame->r.xyz[0] = p[i_state++] * SCALE_ROTATION_FRAME;
    frame->r.xyz[1] = p[i_state++] * SCALE_ROTATION_FRAME;
    frame->r.xyz[2] = p[i_state++] * SCALE_ROTATION_FRAME;

    frame->t.xyz[0] = p[i_state++] * SCALE_TRANSLATION_FRAME;
    frame->t.xyz[1] = p[i_state++] * SCALE_TRANSLATION_FRAME;
    frame->t.xyz[2] = p[i_state++] * SCALE_TRANSLATION_FRAME;
    return i_state;

}

static int unpack_solver_state_point_one(// out
                                         point3_t* point,

                                         // in
                                         const double* p)
{
    int i_state = 0;
    point->xyz[0] = p[i_state++] * SCALE_POSITION_POINT;
    point->xyz[1] = p[i_state++] * SCALE_POSITION_POINT;
    point->xyz[2] = p[i_state++] * SCALE_POSITION_POINT;
    return i_state;
}

static int unpack_solver_state_calobject_warp(// out
                                              point2_t* calobject_warp,

                                              // in
                                              const double* p)
{
    int i_state = 0;
    calobject_warp->xy[0] = p[i_state++] * SCALE_CALOBJECT_WARP;
    calobject_warp->xy[1] = p[i_state++] * SCALE_CALOBJECT_WARP;
    return i_state;
}

// From unit-scale values to real values. Optimizer sees unit-scale values
static void unpack_solver_state( // out
                                 double* intrinsics, // Ncameras of these

                                 pose_t*       extrinsics, // Ncameras-1 of these
                                 pose_t*       frames,     // Nframes of these
                                 point3_t*     points,     // Npoints of these
                                 point2_t*     calobject_warp, // 1 of these

                                 // in
                                 const double* p,
                                 const lensmodel_t lensmodel,
                                 mrcal_problem_details_t problem_details,
                                 int Ncameras, int Nframes, int Npoints,

                                 int Nstate_ref)
{
    int i_state = unpack_solver_state_intrinsics(intrinsics,
                                                 p, lensmodel, problem_details, Ncameras);

    if( problem_details.do_optimize_extrinsics )
        for(int i_camera=1; i_camera < Ncameras; i_camera++)
            i_state += unpack_solver_state_extrinsics_one( &extrinsics[i_camera-1], &p[i_state] );

    if( problem_details.do_optimize_frames )
    {
        for(int i_frame = 0; i_frame < Nframes; i_frame++)
            i_state += unpack_solver_state_framert_one( &frames[i_frame], &p[i_state] );
        for(int i_point = 0; i_point < Npoints; i_point++)
            i_state += unpack_solver_state_point_one( &points[i_point], &p[i_state] );
    }

    if( problem_details.do_optimize_calobject_warp )
        i_state += unpack_solver_state_calobject_warp(calobject_warp, &p[i_state]);

    assert(i_state == Nstate_ref);
}
// Same as above, but packs/unpacks a vector instead of structures
void mrcal_unpack_solver_state_vector( // out, in
                                       double* p, // unitless state on input,
                                                  // scaled, meaningful state on
                                                  // output

                                       // in
                                       const lensmodel_t lensmodel,
                                       mrcal_problem_details_t problem_details,
                                       int Ncameras, int Nframes, int Npoints)
{
    int i_state = unpack_solver_state_intrinsics(p,
                                                 p, lensmodel, problem_details, Ncameras);

    if( problem_details.do_optimize_extrinsics )
    {
        static_assert( offsetof(pose_t, r) == 0,
                       "pose_t has expected structure");
        static_assert( offsetof(pose_t, t) == 3*sizeof(double),
                       "pose_t has expected structure");

        pose_t* extrinsics = (pose_t*)(&p[i_state]);
        for(int i_camera=1; i_camera < Ncameras; i_camera++)
            i_state += unpack_solver_state_extrinsics_one( &extrinsics[i_camera-1], &p[i_state] );
    }

    if( problem_details.do_optimize_frames )
    {
        pose_t* frames = (pose_t*)(&p[i_state]);
        for(int i_frame = 0; i_frame < Nframes; i_frame++)
            i_state += unpack_solver_state_framert_one( &frames[i_frame], &p[i_state] );
        point3_t* points = (point3_t*)(&p[i_state]);
        for(int i_point = 0; i_point < Npoints; i_point++)
            i_state += unpack_solver_state_point_one( &points[i_point], &p[i_state] );
    }
    if( problem_details.do_optimize_calobject_warp )
    {
        point2_t* calobject_warp = (point2_t*)(&p[i_state]);
        i_state += unpack_solver_state_calobject_warp(calobject_warp, &p[i_state]);
    }
}

int mrcal_state_index_intrinsics(int i_camera,
                                 mrcal_problem_details_t problem_details,
                                 lensmodel_t lensmodel)
{
    return i_camera * mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel);
}
int mrcal_state_index_camera_rt(int i_camera, int Ncameras,
                                mrcal_problem_details_t problem_details,
                                lensmodel_t lensmodel)
{
    // returns a bogus value if i_camera==0. This camera has no state, and is
    // assumed to be at identity. The caller must know to not use the return
    // value in that case
    int i = mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel)*Ncameras;
    return i + (i_camera-1)*6;
}
int mrcal_state_index_frame_rt(int i_frame, int Ncameras,
                               mrcal_problem_details_t problem_details,
                               lensmodel_t lensmodel)
{
    return
        Ncameras * mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel) +
        (problem_details.do_optimize_extrinsics ? ((Ncameras-1) * 6) : 0) +
        i_frame * 6;
}
int mrcal_state_index_point(int i_point, int Nframes, int Ncameras,
                            mrcal_problem_details_t problem_details,
                            lensmodel_t lensmodel)
{
    return
        Ncameras * mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel) +
        (problem_details.do_optimize_extrinsics ? ((Ncameras-1) * 6) : 0) +
        (Nframes * 6) +
        i_point*3;
}
int mrcal_state_index_calobject_warp(int Npoints,
                                     int Nframes, int Ncameras,
                                     mrcal_problem_details_t problem_details,
                                     lensmodel_t lensmodel)
{
    return mrcal_state_index_point(Npoints, Nframes,  Ncameras,
                                   problem_details,
                                   lensmodel);
}

// This function is part of sensitivity analysis to quantify how much errors in
// the input pixel observations affect our solution. A "good" solution will not
// be very sensitive: measurement noise doesn't affect the solution very much.
//
// A detailed derivation appears in the docstring for
// compute_intrinsics_uncertainty() in utils.py. Everything is double-checked in
// check_confidence_computations() in mrcal-calibrate-cameras
//
// My matrices are large and sparse. Thus I compute the blocks of M Mt that I
// need here, and return these densely to the upper levels (python). These
// callers will then use these dense matrices to finish the computation
//
//   M Mt = sum(outer(col(M), col(M)))
//   col(M) = solve(JtJ, row(J))
//
// Note that libdogleg sees everything in the unitless space of scaled
// parameters, and I want this scaling business to be contained in the C code,
// and to not leak out to python. Let's say I have parameters p and their
// unitless scaled versions p*. dp = D dp*. So Var(dp) = D Var(dp*) D. So when
// talking to the upper level, I need to report M = DM*.
//
// The returned matrices are symmetric, but I return both halves for now
static bool computeUncertaintyMatrices(// out
                                       // dimensions (Ncameras,Nintrinsics_per_camera,Nintrinsics_per_camera)
                                       double* covariance_intrinsics_full,
                                       double* covariance_intrinsics,
                                       // dimensions ((Ncameras-1)*6,(Ncameras-1)*6)
                                       double* covariance_extrinsics,

                                       // in
                                       double observed_pixel_uncertainty,
                                       lensmodel_t lensmodel,
                                       mrcal_problem_details_t problem_details,
                                       int Ncameras,
                                       int NobservationsBoard,
                                       int NobservationsPoint,
                                       int Nframes, int Npoints,
                                       int calibration_object_width_n,

                                       dogleg_solverContext_t* solverCtx)
{
    // for reading cholmod_sparse
#define P(A, index) ((unsigned int*)((A)->p))[index]
#define I(A, index) ((unsigned int*)((A)->i))[index]
#define X(A, index) ((double*      )((A)->x))[index]

    if(NobservationsBoard <= 0)
        return false;

    //Nintrinsics_per_camera_state can be < Nintrinsics_per_camera_all, if we're
    //locking down some variables with problem_details
    int Nintrinsics_per_camera_all = mrcal_getNlensParams(lensmodel);
    int Nintrinsics_per_camera_state =
        mrcal_getNintrinsicOptimizationParams(problem_details, lensmodel);
    int Nmeas_observations = getNmeasurements_observationsonly(NobservationsBoard,
                                                               NobservationsPoint,
                                                               calibration_object_width_n);

    if(covariance_intrinsics)
        memset(covariance_intrinsics, 0,
               Ncameras*Nintrinsics_per_camera_all* Nintrinsics_per_camera_all*sizeof(double));
    // this one isn't strictly necessary (the computation isn't incremental), but
    // it keeps the logic simple
    if(covariance_intrinsics_full)
        memset(covariance_intrinsics_full, 0,
               Ncameras*Nintrinsics_per_camera_all* Nintrinsics_per_camera_all*sizeof(double));
    if(covariance_extrinsics)
        memset(covariance_extrinsics, 0,
               6*(Ncameras-1) * 6*(Ncameras-1) * sizeof(double));

    if( !(problem_details.do_optimize_intrinsic_core        ||
          problem_details.do_optimize_intrinsic_distortions ||
          problem_details.do_optimize_extrinsics) )
    {
        // We're not optimizing anything I care about here
        return true;
    }

    if( !(covariance_intrinsics_full ||
          covariance_intrinsics ||
          covariance_extrinsics))
    {
        // no buffers to fill in
        return true;
    }

    if( !problem_details.do_optimize_extrinsics &&
        covariance_extrinsics)
    {
        covariance_extrinsics = NULL;
    }

    if( covariance_intrinsics_full || covariance_intrinsics)
    {
        if( !problem_details.do_optimize_intrinsic_core        &&
            !problem_details.do_optimize_intrinsic_distortions )
        {
            covariance_intrinsics_full = covariance_intrinsics = NULL;
        }
        else if( (!problem_details.do_optimize_intrinsic_core        &&
                   problem_details.do_optimize_intrinsic_distortions ) ||
                 ( problem_details.do_optimize_intrinsic_core        &&
                  !problem_details.do_optimize_intrinsic_distortions ) )
        {
            MSG("Can't compute any covariance_intrinsics_... if we aren't optimizing the WHOLE intrinsics: core and distortions");
            return false;
        }

    }

    cholmod_sparse* Jt     = solverCtx->beforeStep->Jt;
    int             Nstate = Jt->nrow;
    int             Nmeas  = Jt->ncol;

    // I will repeatedly solve the system JtJ x = v. CHOLMOD can do this for me
    // quickly, if I pre-analyze and pre-factorize JtJ. I do this here, and then
    // just do the cholmod_solve() in the loop. I just ran the solver, so the
    // analysis and factorization are almost certainly already done. But just in
    // case...
    {
        if(solverCtx->factorization == NULL)
        {
            solverCtx->factorization = cholmod_analyze(Jt, &solverCtx->common);
            MSG("Couldn't factor JtJ");
            return false;
        }

        assert( cholmod_factorize(Jt, solverCtx->factorization, &solverCtx->common) );
        if(solverCtx->factorization->minor != solverCtx->factorization->n)
        {
            MSG("Got singular JtJ!");
            return false;
        }
    }

    // cholmod_spsolve works in chunks of 4, so I do this in chunks of 4 too. I
    // pass it rows of J, 4 at a time. I don't actually allocate anything, rather
    // using views into Jt. So I copy the Jt structure and use that
    const int chunk_size = 4;

    cholmod_dense* Jt_slice =
        cholmod_allocate_dense( Jt->nrow,
                                chunk_size,
                                Jt->nrow,
                                CHOLMOD_REAL,
                                &solverCtx->common );

    // As described above, I'm looking at what input noise does, so I only look
    // at the measurements that pertain to the input observations directly. In
    // mrcal, this is the leading ones, before the range errors and the
    // regularization

    // Compute covariance_intrinsics_full. This is the intrinsics-per-camera
    // diagonal block inv(JtJ) for each camera separately
    if(covariance_intrinsics_full)
        for(int icam = 0; icam < Ncameras; icam++)
        {
            // Here I want the diagonal blocks of inv(JtJ) for each camera's
            // intrinsics. I get them by doing solve(JtJ, [0; I; 0])
            void compute_invJtJ_block(double* invJtJ, const int istate0, int N)
            {
                // I'm solving JtJ x = b where J is sparse, b is sparse, but x ends up
                // dense. cholmod doesn't have functions for this exact case. so I use
                // the dense-sparse-dense function, and densify the input. Instead of
                // sparse-sparse-sparse and the densifying the output. This feels like
                // it'd be more efficient

                int istate = istate0;

                // I can do chunk_size cols at a time
                while(1)
                {
                    int Ncols = N < chunk_size ? N : chunk_size;
                    Jt_slice->ncol = Ncols;
                    memset( Jt_slice->x, 0, Jt_slice->nrow*Ncols*sizeof(double) );
                    for(int icol=0; icol<Ncols; icol++)
                        // The J are unitless. I need to scale them to get real units
                        ((double*)Jt_slice->x)[ istate + icol + icol*Jt_slice->nrow] =
                            get_scale_solver_state_intrinsics_onecamera(istate + icol - istate0,
                                                                        Nintrinsics_per_camera_state,
                                                                        problem_details);

                    cholmod_dense* M = cholmod_solve( CHOLMOD_A, solverCtx->factorization,
                                                      Jt_slice,
                                                      &solverCtx->common);

                    // The cols/rows I want are in M. I pull them out, and apply
                    // scaling (because my J are unitless, and I want full-unit
                    // data)
                    for(int icol=0; icol<Ncols; icol++)
                        unpack_solver_state_intrinsics_onecamera( (intrinsics_core_t*)&invJtJ[icol*Nintrinsics_per_camera_state],
                                                                  lensmodel,
                                                                  &invJtJ[icol*Nintrinsics_per_camera_state + 4],

                                                                  &((double*)(M->x))[icol*M->nrow + istate0],
                                                                  Nintrinsics_per_camera_state,
                                                                  problem_details );
                    cholmod_free_dense (&M, &solverCtx->common);

                    N -= Ncols;
                    if(N <= 0) break;
                    istate += Ncols;
                    invJtJ = &invJtJ[Ncols*Nintrinsics_per_camera_state];
                }
            }




            const int istate0 = Nintrinsics_per_camera_state * icam;
            double* invJtJ_thiscam = &covariance_intrinsics_full[icam*Nintrinsics_per_camera_all*Nintrinsics_per_camera_all];
            compute_invJtJ_block( invJtJ_thiscam, istate0, Nintrinsics_per_camera_state );
        }

    // Compute covariance_intrinsics. This is the
    // intrinsics-per-camera diagonal block
    //   inv(JtJ)[intrinsics] Jobservationst Jobservations inv(JtJ)[intrinsics]t
    // for each camera separately
    //
    // And also compute covariance_extrinsics. This is similar, except all the
    // extrinsics together are reported as a single diagonal block
    if(covariance_intrinsics || covariance_extrinsics)
    {
        int istate_intrinsics0 = mrcal_state_index_intrinsics(0,
                                                              problem_details,
                                                              lensmodel);
        int istate_extrinsics0 = mrcal_state_index_camera_rt(1, Ncameras,
                                                             problem_details,
                                                             lensmodel);

        for(int i_meas=0; i_meas < Nmeas_observations; i_meas += chunk_size)
        {
            // sparse to dense for a chunk of Jt
            memset( Jt_slice->x, 0, Jt_slice->nrow*chunk_size*sizeof(double) );
            for(unsigned int icol=0; icol<(unsigned)chunk_size; icol++)
            {
                if( (int)(i_meas + icol) >= Nmeas_observations )
                {
                    // at the end, we could have one chunk with less that chunk_size
                    // columns
                    Jt_slice->ncol = icol;
                    break;
                }

                for(unsigned int i0=P(Jt, icol+i_meas); i0<P(Jt, icol+i_meas+1); i0++)
                {
                    int irow = I(Jt,i0);
                    double x0 = X(Jt,i0);
                    ((double*)Jt_slice->x)[irow + icol*Jt_slice->nrow] = x0;
                }
            }

            // I'm solving JtJ x = b where J is sparse, b is sparse, but x ends up
            // dense. cholmod doesn't have functions for this exact case. so I use
            // the dense-sparse-dense function, and densify the input. Instead of
            // sparse-sparse-sparse and the densifying the output. This feels like
            // it'd be more efficient
            cholmod_dense* M = cholmod_solve( CHOLMOD_A, solverCtx->factorization,
                                              Jt_slice,
                                              &solverCtx->common);

            // I now have chunk_size columns of M. I accumulate sum of the outer
            // products. This is symmetric, but I store both halves; for now
            for(unsigned int icol=0; icol<M->ncol; icol++)
            {
                // the unpack_solver_state_vector() call assumes that the only
                // difference between the packed and unpacked vectors is the
                // scaling. problem_details could make the contents vary in other
                // ways, and I make sure this doesn't happen. It's possible to make
                // this work in general, but it's tricky, and I don't need to spent
                // the time right now. This will fail if I'm locking down some
                // parameters
                assert(Nintrinsics_per_camera_all == Nintrinsics_per_camera_state);

                // The M I have here is a unitless, scaled M*. I need to scale it to get
                // M. See comment above.
                mrcal_unpack_solver_state_vector( &((double*)(M->x))[icol*M->nrow],
                                                  lensmodel,
                                                  problem_details,
                                                  Ncameras, Nframes, Npoints);

                void accumulate_invJtJ(double* invJtJ, unsigned int irow_chunk_start, unsigned int Nstate_chunk)
                {
                    for(unsigned int irow0=irow_chunk_start;
                        irow0<irow_chunk_start+Nstate_chunk;
                        irow0++)
                    {
                        int i_intrinsics0 = irow0 - irow_chunk_start;
                        // special-case process the diagonal param
                        double x0 = ((double*)(M->x))[irow0 + icol*M->nrow];
                        invJtJ[(Nstate_chunk+1)*i_intrinsics0] += x0*x0;

                        // Now the off-diagonal

                        // I want to look at each camera individually, so I ignore the
                        // interactions between the parameters across cameras
                        for(unsigned int irow1=irow0+1;
                            irow1<irow_chunk_start+Nstate_chunk;
                            irow1++)
                        {
                            double x1 = ((double*)(M->x))[irow1 + icol*M->nrow];
                            double x0x1 = x0*x1;
                            int i_intrinsics1 = irow1 - irow_chunk_start;
                            invJtJ[Nstate_chunk*i_intrinsics0 + i_intrinsics1] += x0x1;
                            invJtJ[Nstate_chunk*i_intrinsics1 + i_intrinsics0] += x0x1;
                        }
                    }
                }


                // Intrinsics. Each camera into a separate inv(JtJ) block
                if(covariance_intrinsics)
                    for(int icam=0; icam<Ncameras; icam++)
                    {
                        double* invJtJ_thiscam =
                            &covariance_intrinsics[icam*Nintrinsics_per_camera_all*Nintrinsics_per_camera_all];
                        accumulate_invJtJ(invJtJ_thiscam,
                                          istate_intrinsics0 + icam * Nintrinsics_per_camera_state,
                                          Nintrinsics_per_camera_state);
                    }

                // Extrinsics. Everything into one big inv(JtJ) block
                if(covariance_extrinsics)
                    accumulate_invJtJ(covariance_extrinsics,
                                      istate_extrinsics0,
                                      6 * (Ncameras-1));

            }

            cholmod_free_dense (&M, &solverCtx->common);
        }
    }

    Jt_slice->ncol = chunk_size; // I manually reset this earlier; put it back
    cholmod_free_dense(&Jt_slice, &solverCtx->common);


    // I computed inv(JtJ). I now scale it to form a covariance
    double s = observed_pixel_uncertainty*observed_pixel_uncertainty;
    if(covariance_intrinsics)
        for(int i=0;
            i<Ncameras*Nintrinsics_per_camera_all*Nintrinsics_per_camera_all;
            i++)
            covariance_intrinsics[i] *= s;
    if(covariance_intrinsics_full)
        for(int i=0;
            i<Ncameras*Nintrinsics_per_camera_all* Nintrinsics_per_camera_all;
            i++)
            covariance_intrinsics_full[i] *= s;
    if(covariance_extrinsics)
        for(int i=0;
            i<6*(Ncameras-1) * 6*(Ncameras-1);
            i++)
            covariance_extrinsics[i] *= s;

    return true;

#undef P
#undef I
#undef X
}

// Doing this myself instead of hooking into the logic in libdogleg. THIS
// implementation is simpler (looks just at the residuals), but also knows to
// ignore the outside-ROI data
static
bool markOutliers(// output, input
                  struct dogleg_outliers_t* markedOutliers,

                  // output
                  int* Noutliers,

                  // input
                  const observation_board_t* observations_board,
                  int NobservationsBoard,
                  int calibration_object_width_n,
                  const double* roi,

                  const double* x_measurements,
                  double expected_xy_stdev,
                  bool verbose)
{
    // I define an outlier as a feature that's > k stdevs past the mean. The
    // threshold stdev is the worse of
    // - The stdev of my data set
    // - The expected stdev of my noise (calibrate-cameras
    //   --observed-pixel-uncertainty)
    //
    // The rationale:
    //
    // - If I have a really good solve, the stdev of my data set will be very
    //   low, so deviations off that already-very-good solve aren't important. I
    //   use the expected-noise stdev in this case
    //
    // - If the solve isn't great, my errors may exceed the expected-noise stdev
    //   (if my model doesn't fit very well, say). In that case I want to use
    //   the stdev from the data

    // threshold. +- 3sigma includes 99.7% of the data in a normal distribution
    const double k = 3.0;

#warning "think about this. here I'm looking at the deviations off mean error. That sounds wrong. Do I care about mean error? I want error to be 0, so maybe looking at absolute error is the thing to do instead"

    *Noutliers = 0;

    int i_pt,i_feature;


#define LOOP_FEATURE_BEGIN()                                            \
    i_feature = 0;                                                      \
    for(int i_observation_board=0;                                      \
        i_observation_board<NobservationsBoard;                         \
        i_observation_board++)                                          \
    {                                                                   \
        const observation_board_t* observation = &observations_board[i_observation_board]; \
        const int i_camera = observation->i_camera;                     \
        for(i_pt=0;                                                     \
            i_pt < calibration_object_width_n*calibration_object_width_n; \
            i_pt++, i_feature++)                                        \
        {                                                               \
            const point3_t* pt_observed = &observation->px[i_pt]; \
            double weight_roi = region_of_interest_weight(pt_observed, roi, i_camera); \
            double weight = weight_roi * pt_observed->z;


#define LOOP_FEATURE_END() \
    }}


    // I loop through my data 3 times: 2 times to compute the stdev, and then
    // once more to use that value to identify the outliers

    double sum_mean   = 0.0;
    double sum_weight = 0.0;
    LOOP_FEATURE_BEGIN()
    {
        if(markedOutliers[i_feature].marked)
        {
          (*Noutliers)++;
          continue;
        }
        if(weight_roi != 1.0) continue;

        sum_mean +=
            weight *
            (x_measurements[2*i_feature + 0] +
             x_measurements[2*i_feature + 1]);
        sum_weight += weight;
    }
    LOOP_FEATURE_END();
    sum_mean /= (2. * sum_weight);

    double var = 0.0;
    LOOP_FEATURE_BEGIN()
    {
        if(markedOutliers[i_feature].marked)
          continue;
        if(weight_roi != 1.0) continue;

        double dx = (x_measurements[2*i_feature + 0] - sum_mean);
        double dy = (x_measurements[2*i_feature + 1] - sum_mean);

        var += weight*(dx*dx + dy*dy);
    }
    LOOP_FEATURE_END();
    var /= (2.*sum_weight);

    if(var < expected_xy_stdev*expected_xy_stdev)
        var = expected_xy_stdev*expected_xy_stdev;

    bool markedAny = false;
    LOOP_FEATURE_BEGIN()
    {
        if(markedOutliers[i_feature].marked)
          continue;
        if(weight_roi != 1.0) continue;

        double dx = (x_measurements[2*i_feature + 0] - sum_mean);
        double dy = (x_measurements[2*i_feature + 1] - sum_mean);
        if(dx*dx > k*k*var || dy*dy > k*k*var )
        {
            markedOutliers[i_feature].marked = true;
            markedAny                        = true;
            (*Noutliers)++;

            // MSG_IF_VERBOSE("Feature %d looks like an outlier. x/y are %f/%f stdevs off mean. Observed stdev: %f, limit: %f",
            //                i_feature, dx/sqrt(var), dy/sqrt(var), sqrt(var), k);

        }
    }
    LOOP_FEATURE_END();

    return markedAny;

#undef LOOP_FEATURE_BEGIN
#undef LOOP_FEATURE_END
}

typedef struct
{
    // these are all UNPACKED
    const double*       intrinsics; // Ncameras * NlensParams of these
    const pose_t*       extrinsics; // Ncameras-1 of these. Transform FROM camera0 frame
    const pose_t*       frames;     // Nframes of these.    Transform TO   camera0 frame
    const point3_t*     points;     // Npoints of these.    In the camera0 frame
    const point2_t*     calobject_warp; // 1 of these. May be NULL if !problem_details.do_optimize_calobject_warp

    // in
    int Ncameras, Nframes, Npoints;

    const observation_board_t* observations_board;
    int NobservationsBoard;

    const observation_point_t* observations_point;
    int NobservationsPoint;

    bool verbose;

    lensmodel_t lensmodel;
    const int* imagersizes; // Ncameras*2 of these

    mrcal_problem_details_t problem_details;

    double calibration_object_spacing;
    int calibration_object_width_n;

    const double* roi;
    const int Nmeasurements, N_j_nonzero, Nintrinsics;
    struct dogleg_outliers_t* markedOutliers;
    const char* reportFitMsg;
} callback_context_t;

static
void optimizerCallback(// input state
                       const double*   packed_state,

                       // output measurements
                       double*         x,

                       // Jacobian
                       cholmod_sparse* Jt,

                       const callback_context_t* ctx)
{
    double norm2_error = 0.0;

    int    iJacobian          = 0;
    int    iMeasurement       = 0;

    int*    Jrowptr = Jt ? (int*)   Jt->p : NULL;
    int*    Jcolidx = Jt ? (int*)   Jt->i : NULL;
    double* Jval    = Jt ? (double*)Jt->x : NULL;
#define STORE_JACOBIAN(col, g)                  \
    do                                          \
    {                                           \
        if(Jt) {                                \
            Jcolidx[ iJacobian ] = col;         \
            Jval   [ iJacobian ] = g;           \
        }                                       \
        iJacobian++;                            \
    } while(0)
#define STORE_JACOBIAN2(col0, g0, g1)           \
    do                                          \
    {                                           \
        if(Jt) {                                \
            Jcolidx[ iJacobian+0 ] = col0+0;    \
            Jval   [ iJacobian+0 ] = g0;        \
            Jcolidx[ iJacobian+1 ] = col0+1;    \
            Jval   [ iJacobian+1 ] = g1;        \
        }                                       \
        iJacobian += 2;                         \
    } while(0)
    #define STORE_JACOBIAN3(col0, g0, g1, g2)               \
        do                                              \
        {                                               \
            if(Jt) {                                    \
                Jcolidx[ iJacobian+0 ] = col0+0;        \
                Jval   [ iJacobian+0 ] = g0;            \
                Jcolidx[ iJacobian+1 ] = col0+1;        \
                Jval   [ iJacobian+1 ] = g1;            \
                Jcolidx[ iJacobian+2 ] = col0+2;        \
                Jval   [ iJacobian+2 ] = g2;            \
            }                                           \
            iJacobian += 3;                             \
        } while(0)


    int Ncore = mrcal_modelHasCore_fxfycxcy(ctx->lensmodel) ? 4 : 0;
    int Ncore_state = (mrcal_modelHasCore_fxfycxcy(ctx->lensmodel) &&
                       ctx->problem_details.do_optimize_intrinsic_core) ? 4 : 0;

    // If I'm locking down some parameters, then the state vector contains a
    // subset of my data. I reconstitute the intrinsics and extrinsics here.
    // I do the frame poses later. This is a good way to do it if I have few
    // cameras. With many cameras (this will be slow)
    double intrinsics_all[ctx->Ncameras][ctx->Nintrinsics];
    pose_t camera_rt[ctx->Ncameras];

    point2_t calobject_warp_local = {};
    const int i_var_calobject_warp = mrcal_state_index_calobject_warp(ctx->Npoints, ctx->Nframes, ctx->Ncameras, ctx->problem_details, ctx->lensmodel);
    if(ctx->problem_details.do_optimize_calobject_warp)
        unpack_solver_state_calobject_warp(&calobject_warp_local, &packed_state[i_var_calobject_warp]);
    else if(ctx->calobject_warp != NULL)
        calobject_warp_local = *ctx->calobject_warp;

    for(int i_camera=0; i_camera<ctx->Ncameras; i_camera++)
    {
        // First I pull in the chunks from the optimization vector
        const int i_var_intrinsics = mrcal_state_index_intrinsics(i_camera,                ctx->problem_details, ctx->lensmodel);
        const int i_var_camera_rt  = mrcal_state_index_camera_rt (i_camera, ctx->Ncameras, ctx->problem_details, ctx->lensmodel);

        double* intrinsics_here  = &intrinsics_all[i_camera][0];
        double* distortions_here = &intrinsics_all[i_camera][Ncore];

        unpack_solver_state_intrinsics_onecamera((intrinsics_core_t*)intrinsics_here,
                                                 ctx->lensmodel,
                                                 distortions_here,
                                                 &packed_state[ i_var_intrinsics ],
                                                 ctx->Nintrinsics,
                                                 ctx->problem_details );

        // And then I fill in the gaps using the seed values
        if(!ctx->problem_details.do_optimize_intrinsic_core && Ncore)
            memcpy( intrinsics_here,
                    &ctx->intrinsics[ctx->Nintrinsics*i_camera],
                    Ncore*sizeof(double) );
        if(!ctx->problem_details.do_optimize_intrinsic_distortions)
            memcpy( distortions_here,
                    &ctx->intrinsics[ctx->Nintrinsics*i_camera + Ncore],
                    (ctx->Nintrinsics-Ncore)*sizeof(double) );

        // extrinsics
        if( i_camera != 0 )
        {
            if(ctx->problem_details.do_optimize_extrinsics)
                unpack_solver_state_extrinsics_one(&camera_rt[i_camera-1], &packed_state[i_var_camera_rt]);
            else
                memcpy(&camera_rt[i_camera-1], &ctx->extrinsics[i_camera-1], sizeof(pose_t));
        }
    }

    for(int i_observation_board = 0;
        i_observation_board < ctx->NobservationsBoard;
        i_observation_board++)
    {
        const observation_board_t* observation = &ctx->observations_board[i_observation_board];

        const int i_camera = observation->i_camera;
        const int i_frame  = observation->i_frame;


        // Some of these are bogus if problem_details says they're inactive
        const int i_var_frame_rt = mrcal_state_index_frame_rt  (i_frame,  ctx->Ncameras, ctx->problem_details, ctx->lensmodel);

        pose_t frame_rt;
        if(ctx->problem_details.do_optimize_frames)
            unpack_solver_state_framert_one(&frame_rt, &packed_state[i_var_frame_rt]);
        else
            memcpy(&frame_rt, &ctx->frames[i_frame], sizeof(pose_t));

        const int i_var_intrinsics = mrcal_state_index_intrinsics(i_camera,                ctx->problem_details, ctx->lensmodel);
        const int i_var_camera_rt  = mrcal_state_index_camera_rt (i_camera, ctx->Ncameras, ctx->problem_details, ctx->lensmodel);

        // these are computed in respect to the real-unit parameters,
        // NOT the unit-scale parameters used by the optimizer
        point3_t dxy_drcamera       [ctx->calibration_object_width_n*ctx->calibration_object_width_n][2];
        point3_t dxy_dtcamera       [ctx->calibration_object_width_n*ctx->calibration_object_width_n][2];
        point3_t dxy_drframe        [ctx->calibration_object_width_n*ctx->calibration_object_width_n][2];
        point3_t dxy_dtframe        [ctx->calibration_object_width_n*ctx->calibration_object_width_n][2];
        point2_t dxy_dcalobject_warp[ctx->calibration_object_width_n*ctx->calibration_object_width_n][2];
        point2_t pt_hypothesis      [ctx->calibration_object_width_n*ctx->calibration_object_width_n];

        // I get the intrinsics gradients in separate arrays, possibly sparsely.
        // All the data lives in dxy_dintrinsics_pool_double[], with the other data
        // indicating the meaning of the values in the pool.
        //
        // dxy_dfxy serves a special-case for a perspective core. Such models
        // are very common, and they have x = fx vx/vz + cx and y = fy vy/vz +
        // cy. So x depends on fx and NOT on fy, and similarly for y. Similar
        // for cx,cy, except we know the gradient value beforehand. I support
        // this case explicitly here. I store dx/dfx and dy/dfy; no cross terms
        double dxy_dintrinsics_pool_double[ctx->calibration_object_width_n*ctx->calibration_object_width_n*2*(1+ctx->Nintrinsics-4)];
        int    dxy_dintrinsics_pool_int   [ctx->calibration_object_width_n*ctx->calibration_object_width_n];
        double* dxy_dfxy = NULL;
        double* dxy_dintrinsics_nocore = NULL;
        gradient_sparse_meta_t gradient_sparse_meta = {}; // init to pacify compiler warning

        splined_intrinsics_grad_context_t* splined_intrinsics_grad_context =
            (splined_intrinsics_grad_context_t*)dxy_dintrinsics_pool_double;
        int splined_intrinsics_grad_irun = 0;

        project(pt_hypothesis,

                ctx->problem_details.do_optimize_intrinsic_core || ctx->problem_details.do_optimize_intrinsic_distortions ?
                  dxy_dintrinsics_pool_double : NULL,
                ctx->problem_details.do_optimize_intrinsic_core || ctx->problem_details.do_optimize_intrinsic_distortions ?
                  dxy_dintrinsics_pool_int : NULL,
                &dxy_dfxy, &dxy_dintrinsics_nocore, &gradient_sparse_meta,

                ctx->problem_details.do_optimize_extrinsics ?
                (point3_t*)dxy_drcamera : NULL,
                ctx->problem_details.do_optimize_extrinsics ?
                (point3_t*)dxy_dtcamera : NULL,
                ctx->problem_details.do_optimize_frames ?
                (point3_t*)dxy_drframe : NULL,
                ctx->problem_details.do_optimize_frames ?
                (point3_t*)dxy_dtframe : NULL,
                ctx->problem_details.do_optimize_calobject_warp ?
                (point2_t*)dxy_dcalobject_warp : NULL,
                intrinsics_all[i_camera],
                &camera_rt[i_camera-1], &frame_rt,
                ctx->calobject_warp == NULL ? NULL : &calobject_warp_local,
                i_camera == 0,
                ctx->lensmodel,
                ctx->calibration_object_spacing,
                ctx->calibration_object_width_n);

        for(int i_pt=0;
            i_pt < ctx->calibration_object_width_n*ctx->calibration_object_width_n;
            i_pt++)
        {
            const point3_t* pt_observed = &observation->px[i_pt];
            double weight = region_of_interest_weight(pt_observed, ctx->roi, i_camera);
            weight *= pt_observed->z;

            if(!observation->skip_observation &&

               // /2 because I look at FEATURES here, not discrete
               // measurements
               !ctx->markedOutliers[iMeasurement/2].marked)
            {
                // I have my two measurements (dx, dy). I propagate their
                // gradient and store them
                for( int i_xy=0; i_xy<2; i_xy++ )
                {
                    const double err = (pt_hypothesis[i_pt].xy[i_xy] - pt_observed->xyz[i_xy]) * weight;

                    if( ctx->reportFitMsg )
                    {
                        MSG("%s: obs/frame/cam/dot: %d %d %d %d err: %g",
                            ctx->reportFitMsg,
                            i_observation_board, i_frame, i_camera, i_pt, err);
                        continue;
                    }

                    if(Jt) Jrowptr[iMeasurement] = iJacobian;
                    x[iMeasurement] = err;
                    norm2_error += err*err;

                    if( ctx->problem_details.do_optimize_intrinsic_core )
                    {
                        // fx,fy. x depends on fx only. y depends on fy only
                        STORE_JACOBIAN( i_var_intrinsics + i_xy,
                                        dxy_dfxy[i_pt*2 + i_xy] *
                                        weight * SCALE_INTRINSICS_FOCAL_LENGTH );

                        // cx,cy. The gradients here are known to be 1. And x depends on cx only. And y depends on cy only
                        STORE_JACOBIAN( i_var_intrinsics + i_xy+2,
                                        weight * SCALE_INTRINSICS_CENTER_PIXEL );
                    }

                    if( ctx->problem_details.do_optimize_intrinsic_distortions )
                    {
                        if(gradient_sparse_meta.run_side_length > 0)
                        {
                            // u = stereographic(p)
                            // q = [ ux * fx(u, intrinsics) + cx ]
                            //   = [ uy * fy(u, intrinsics) + cy ]
                            //
                            // Intrinsics:
                            //   dqx/diii = ux dfx/diii
                            //
                            // dfx/diii = flatten(ABCDx[0..3] * ABCDy[0..3])

                            // pool_double contains ABCDxy and u in a
                            // splined_intrinsics_grad_context_t
                            const splined_intrinsics_grad_context_t* ctx =
                                splined_intrinsics_grad_context;
                            const int ivar0 = dxy_dintrinsics_pool_int[splined_intrinsics_grad_irun];

                            const int len          = gradient_sparse_meta.run_side_length;
                            const int ivar_stridey = gradient_sparse_meta.ivar_stridey;

                            for(int iy=0; iy<len; iy++)
                                for(int ix=0; ix<len; ix++)
                                    STORE_JACOBIAN( i_var_intrinsics + ivar0 + iy*ivar_stridey + ix*2 + i_xy,
                                                    ctx->ABCDx[ix]*ctx->ABCDy[iy]*ctx->u.xy[i_xy] *
                                                    weight * SCALE_DISTORTION );
                        }
                        else
                        {
                            for(int i=0; i<ctx->Nintrinsics-Ncore; i++)
                                STORE_JACOBIAN( i_var_intrinsics+Ncore_state + i,
                                                dxy_dintrinsics_nocore[i_pt*2*(ctx->Nintrinsics-Ncore) +
                                                                       i_xy*(ctx->Nintrinsics-Ncore) +
                                                                       i] *
                                                weight * SCALE_DISTORTION );
                        }
                    }

                    if( ctx->problem_details.do_optimize_extrinsics )
                        if( i_camera != 0 )
                        {
                            STORE_JACOBIAN3( i_var_camera_rt + 0,
                                             dxy_drcamera[i_pt][i_xy].xyz[0] *
                                             weight * SCALE_ROTATION_CAMERA,
                                             dxy_drcamera[i_pt][i_xy].xyz[1] *
                                             weight * SCALE_ROTATION_CAMERA,
                                             dxy_drcamera[i_pt][i_xy].xyz[2] *
                                             weight * SCALE_ROTATION_CAMERA);
                            STORE_JACOBIAN3( i_var_camera_rt + 3,
                                             dxy_dtcamera[i_pt][i_xy].xyz[0] *
                                             weight * SCALE_TRANSLATION_CAMERA,
                                             dxy_dtcamera[i_pt][i_xy].xyz[1] *
                                             weight * SCALE_TRANSLATION_CAMERA,
                                             dxy_dtcamera[i_pt][i_xy].xyz[2] *
                                             weight * SCALE_TRANSLATION_CAMERA);
                        }

                    if( ctx->problem_details.do_optimize_frames )
                    {
                        STORE_JACOBIAN3( i_var_frame_rt + 0,
                                         dxy_drframe[i_pt][i_xy].xyz[0] *
                                         weight * SCALE_ROTATION_FRAME,
                                         dxy_drframe[i_pt][i_xy].xyz[1] *
                                         weight * SCALE_ROTATION_FRAME,
                                         dxy_drframe[i_pt][i_xy].xyz[2] *
                                         weight * SCALE_ROTATION_FRAME);
                        STORE_JACOBIAN3( i_var_frame_rt + 3,
                                         dxy_dtframe[i_pt][i_xy].xyz[0] *
                                         weight * SCALE_TRANSLATION_FRAME,
                                         dxy_dtframe[i_pt][i_xy].xyz[1] *
                                         weight * SCALE_TRANSLATION_FRAME,
                                         dxy_dtframe[i_pt][i_xy].xyz[2] *
                                         weight * SCALE_TRANSLATION_FRAME);
                    }

                    if( ctx->problem_details.do_optimize_calobject_warp )
                    {
                        STORE_JACOBIAN2( i_var_calobject_warp,
                                         dxy_dcalobject_warp[i_pt][i_xy].x * weight * SCALE_CALOBJECT_WARP,
                                         dxy_dcalobject_warp[i_pt][i_xy].y * weight * SCALE_CALOBJECT_WARP);
                    }

                    iMeasurement++;
                }
                if(gradient_sparse_meta.run_side_length > 0)
                {
                    splined_intrinsics_grad_context++;
                    splined_intrinsics_grad_irun++;
                }
            }
            else
            {
                // This is arbitrary. I'm skipping this observation, so I
                // don't touch the projection results. I need to have SOME
                // dependency on the frame parameters to ensure a full-rank
                // Hessian. So if we're skipping all observations for this
                // frame, I replace this cost contribution with an L2 cost
                // on the frame parameters.
                for( int i_xy=0; i_xy<2; i_xy++ )
                {
                    const double err = 0.0;

                    if( ctx->reportFitMsg )
                    {
                        MSG( "%s: obs/frame/cam/dot: %d %d %d %d err: %g",
                             ctx->reportFitMsg,
                             i_observation_board, i_frame, i_camera, i_pt, err);
                        continue;
                    }

                    if(Jt) Jrowptr[iMeasurement] = iJacobian;
                    x[iMeasurement] = err;
                    norm2_error += err*err;

                    if( ctx->problem_details.do_optimize_intrinsic_core )
                        for(int i=0; i<4; i++)
                            STORE_JACOBIAN( i_var_intrinsics + i, 0.0 );

                    if( ctx->problem_details.do_optimize_intrinsic_distortions )
                        for(int i=0; i<ctx->Nintrinsics-Ncore; i++)
                            STORE_JACOBIAN( i_var_intrinsics+Ncore_state + i, 0.0 );

                    if( ctx->problem_details.do_optimize_extrinsics )
                        if( i_camera != 0 )
                        {
                            STORE_JACOBIAN3( i_var_camera_rt + 0, 0.0, 0.0, 0.0);
                            STORE_JACOBIAN3( i_var_camera_rt + 3, 0.0, 0.0, 0.0);
                        }

                    if( ctx->problem_details.do_optimize_frames )
                    {
                        const double dframe = observation->skip_frame ? 1.0 : 0.0;
                        // Arbitrary differences between the dimensions to keep
                        // my Hessian non-singular. This is 100% arbitrary. I'm
                        // skipping these measurements so these variables
                        // actually don't affect the computation at all
                        STORE_JACOBIAN3( i_var_frame_rt + 0, dframe*1.1, dframe*1.2, dframe*1.3);
                        STORE_JACOBIAN3( i_var_frame_rt + 3, dframe*1.4, dframe*1.5, dframe*1.6);
                    }

                    if( ctx->problem_details.do_optimize_calobject_warp )
                        STORE_JACOBIAN2( i_var_calobject_warp, 0.0, 0.0 );


                    iMeasurement++;
                }
            }
        }
    }

    // Handle all the point observations. This is VERY similar to the
    // board-observation loop above. Please consolidate
    for(int i_observation_point = 0;
        i_observation_point < ctx->NobservationsPoint;
        i_observation_point++)
    {
        const observation_point_t* observation = &ctx->observations_point[i_observation_point];

        const int i_camera = observation->i_camera;
        const int i_point  = observation->i_point;

        const point3_t* pt_observed = &observation->px;
        double weight = region_of_interest_weight(pt_observed, ctx->roi, i_camera);
        weight *= pt_observed->z;

        const int i_var_intrinsics = mrcal_state_index_intrinsics(i_camera,                              ctx->problem_details, ctx->lensmodel);
        const int i_var_camera_rt  = mrcal_state_index_camera_rt (i_camera, ctx->Ncameras,               ctx->problem_details, ctx->lensmodel);
        const int i_var_point      = mrcal_state_index_point     (i_point,  ctx->Nframes, ctx->Ncameras, ctx->problem_details, ctx->lensmodel);
        point3_t  point;

        if(ctx->problem_details.do_optimize_frames)
            unpack_solver_state_point_one(&point, &packed_state[i_var_point]);
        else
            point = ctx->points[i_point];


        // Check for invalid points. I report a very poor cost if I see
        // this.
        bool have_invalid_point = false;
        if( point.z <= 0.0 || point.z >= POINT_MAXZ )
        {
            have_invalid_point = true;
            if(ctx->verbose)
                MSG( "Saw invalid point distance: z = %g! obs/point/cam: %d %d %d",
                     point.z,
                     i_observation_point, i_point, i_camera);
        }

        double dxy_dintrinsics_pool_double[2*(1+ctx->Nintrinsics-4)];
        int    dxy_dintrinsics_pool_int   [1];
        double* dxy_dfxy;
        double* dxy_dintrinsics_nocore;
        gradient_sparse_meta_t gradient_sparse_meta;

        point3_t dxy_drcamera[2];
        point3_t dxy_dtcamera[2];
        point3_t dxy_dpoint  [2];

        // The array reference [-3] is intended, but the compiler throws a
        // warning. I silence it here
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        point2_t pt_hypothesis;
        project(&pt_hypothesis,

                ctx->problem_details.do_optimize_intrinsic_core || ctx->problem_details.do_optimize_intrinsic_distortions ?
                  dxy_dintrinsics_pool_double : NULL,
                ctx->problem_details.do_optimize_intrinsic_core || ctx->problem_details.do_optimize_intrinsic_distortions ?
                  dxy_dintrinsics_pool_int : NULL,
                &dxy_dfxy, &dxy_dintrinsics_nocore, &gradient_sparse_meta,

                ctx->problem_details.do_optimize_extrinsics ?
                dxy_drcamera : NULL,
                ctx->problem_details.do_optimize_extrinsics ?
                dxy_dtcamera : NULL,
                NULL, // frame rotation. I only have a point position
                ctx->problem_details.do_optimize_frames ?
                dxy_dpoint : NULL,
                NULL,
                intrinsics_all[i_camera],
                &camera_rt[i_camera-1],

                // I only have the point position, so the 'rt' memory
                // points 3 back. The fake "r" here will not be
                // referenced
                (pose_t*)(&point.xyz[-3]),
                NULL,

                i_camera == 0,
                ctx->lensmodel,
                0,0);
#pragma GCC diagnostic pop

        if(!observation->skip_observation
#warning "no outlier rejection on points yet; see warning above"
           )
        {
            // I have my two measurements (dx, dy). I propagate their
            // gradient and store them
            double invalid_point_scale = 1.0;
            if(have_invalid_point)
                // I have an invalid point. This is a VERY bad solution. The solver
                // needs to try again with a smaller step
                invalid_point_scale = 1e6;


            for( int i_xy=0; i_xy<2; i_xy++ )
            {
                const double err = (pt_hypothesis.xy[i_xy] - pt_observed->xyz[i_xy])*invalid_point_scale*weight;

                if(Jt) Jrowptr[iMeasurement] = iJacobian;
                x[iMeasurement] = err;
                norm2_error += err*err;

                if( ctx->problem_details.do_optimize_intrinsic_core )
                {
                    // fx,fy. x depends on fx only. y depends on fy only
                    STORE_JACOBIAN( i_var_intrinsics + i_xy,
                                    dxy_dfxy[i_xy] *
                                    invalid_point_scale *
                                    weight * SCALE_INTRINSICS_FOCAL_LENGTH );

                    // cx,cy. The gradients here are known to be 1. And x depends on cx only. And y depends on cy only
                    STORE_JACOBIAN( i_var_intrinsics + i_xy+2,
                                    invalid_point_scale *
                                    weight * SCALE_INTRINSICS_CENTER_PIXEL );
                }

                if( ctx->problem_details.do_optimize_intrinsic_distortions )
                {
                    if(gradient_sparse_meta.run_side_length > 0)
                    {
                        // u = stereographic(p)
                        // q = [ ux * fx(u, intrinsics) + cx ]
                        //   = [ uy * fy(u, intrinsics) + cy ]
                        //
                        // Intrinsics:
                        //   dqx/diii = ux dfx/diii
                        //
                        // dfx/diii = flatten(ABCDx[0..3] * ABCDy[0..3])

                        // pool_double contains ABCDxy and u in a
                        // splined_intrinsics_grad_context_t
                        const splined_intrinsics_grad_context_t* ctx =
                            (splined_intrinsics_grad_context_t*)dxy_dintrinsics_pool_double;
                        const int ivar0 = dxy_dintrinsics_pool_int[0];

                        const int len          = gradient_sparse_meta.run_side_length;
                        const int ivar_stridey = gradient_sparse_meta.ivar_stridey;

                        for(int iy=0; iy<len; iy++)
                            for(int ix=0; ix<len; ix++)
                            {
                                STORE_JACOBIAN( i_var_intrinsics + ivar0 + iy*ivar_stridey + ix*2 + i_xy,
                                                ctx->ABCDx[ix]*ctx->ABCDy[iy]*ctx->u.xy[i_xy] *
                                                weight * SCALE_DISTORTION );
                            }
                    }
                    else
                    {
                        for(int i=0; i<ctx->Nintrinsics-Ncore; i++)
                            STORE_JACOBIAN( i_var_intrinsics+Ncore_state + i,
                                            dxy_dintrinsics_nocore[i_xy*(ctx->Nintrinsics-Ncore) +
                                                                   i] *
                                            invalid_point_scale *
                                            weight * SCALE_DISTORTION );
                    }
                }

                if( ctx->problem_details.do_optimize_extrinsics )
                    if( i_camera != 0 )
                    {
                        STORE_JACOBIAN3( i_var_camera_rt + 0,
                                         dxy_drcamera[i_xy].xyz[0] *
                                         invalid_point_scale *
                                         weight * SCALE_ROTATION_CAMERA,
                                         dxy_drcamera[i_xy].xyz[1] *
                                         invalid_point_scale *
                                         weight * SCALE_ROTATION_CAMERA,
                                         dxy_drcamera[i_xy].xyz[2] *
                                         invalid_point_scale *
                                         weight * SCALE_ROTATION_CAMERA);
                        STORE_JACOBIAN3( i_var_camera_rt + 3,
                                         dxy_dtcamera[i_xy].xyz[0] *
                                         invalid_point_scale *
                                         weight * SCALE_TRANSLATION_CAMERA,
                                         dxy_dtcamera[i_xy].xyz[1] *
                                         invalid_point_scale *
                                         weight * SCALE_TRANSLATION_CAMERA,
                                         dxy_dtcamera[i_xy].xyz[2] *
                                         invalid_point_scale *
                                         weight * SCALE_TRANSLATION_CAMERA);
                    }

                if( ctx->problem_details.do_optimize_frames )
                    STORE_JACOBIAN3( i_var_point,
                                     dxy_dpoint[i_xy].xyz[0] *
                                     invalid_point_scale *
                                     weight * SCALE_POSITION_POINT,
                                     dxy_dpoint[i_xy].xyz[1] *
                                     invalid_point_scale *
                                     weight * SCALE_POSITION_POINT,
                                     dxy_dpoint[i_xy].xyz[2] *
                                     invalid_point_scale *
                                     weight * SCALE_POSITION_POINT);

                iMeasurement++;
            }

            // Now handle the reference distance, if given
            if( observation->dist > 0.0)
            {
                // I do this in the observing-camera coord system. The
                // camera is at 0. The point is at
                //
                //   Rc*p_point + t

                // This code is copied from project(). PLEASE consolidate
                if(i_camera == 0)
                {
                    double dist = sqrt( point.x*point.x +
                                        point.y*point.y +
                                        point.z*point.z );
                    double err = dist - observation->dist;
                    err *= DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M;

                    if(Jt) Jrowptr[iMeasurement] = iJacobian;
                    x[iMeasurement] = err;
                    norm2_error += err*err;

                    if( ctx->problem_details.do_optimize_frames )
                    {
                        double scale = DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M / dist * SCALE_POSITION_POINT;
                        STORE_JACOBIAN3( i_var_point,
                                         scale*point.x,
                                         scale*point.y,
                                         scale*point.z );
                    }

                    iMeasurement++;
                }
                else
                {
                    // I need to transform the point. I already computed
                    // this stuff in project()...
                    CvMat rc = cvMat(3,1, CV_64FC1, camera_rt[i_camera-1].r.xyz);

                    double _Rc[3*3];
                    CvMat  Rc = cvMat(3,3,CV_64FC1, _Rc);
                    double _d_Rc_rc[9*3];
                    CvMat d_Rc_rc = cvMat(9,3,CV_64F, _d_Rc_rc);
                    cvRodrigues2(&rc, &Rc, &d_Rc_rc);

                    point3_t pt_cam;
                    mul_vec3_gen33t_vout(point.xyz, _Rc, pt_cam.xyz);
                    add_vec(3, pt_cam.xyz, camera_rt[i_camera-1].t.xyz);

                    double dist = sqrt( pt_cam.x*pt_cam.x +
                                        pt_cam.y*pt_cam.y +
                                        pt_cam.z*pt_cam.z );
                    double dist_recip = 1.0/dist;
                    double err = dist - observation->dist;
                    err *= DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M;

                    if(Jt) Jrowptr[iMeasurement] = iJacobian;
                    x[iMeasurement] = err;
                    norm2_error += err*err;

                    if( ctx->problem_details.do_optimize_extrinsics )
                    {
                        // pt_cam.x       = Rc[row0]*point*SCALE + tc
                        // d(pt_cam.x)/dr = d(Rc[row0])/drc*point*SCALE
                        // d(Rc[row0])/drc is 3x3 matrix at &_d_Rc_rc[0]
                        double d_ptcamx_dr[3];
                        double d_ptcamy_dr[3];
                        double d_ptcamz_dr[3];
                        mul_vec3_gen33_vout( point.xyz, &_d_Rc_rc[9*0], d_ptcamx_dr );
                        mul_vec3_gen33_vout( point.xyz, &_d_Rc_rc[9*1], d_ptcamy_dr );
                        mul_vec3_gen33_vout( point.xyz, &_d_Rc_rc[9*2], d_ptcamz_dr );

                        STORE_JACOBIAN3( i_var_camera_rt + 0,
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M *
                                         SCALE_ROTATION_CAMERA*
                                         dist_recip*( pt_cam.x*d_ptcamx_dr[0] +
                                                      pt_cam.y*d_ptcamy_dr[0] +
                                                      pt_cam.z*d_ptcamz_dr[0] ),
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M *
                                         SCALE_ROTATION_CAMERA*
                                         dist_recip*( pt_cam.x*d_ptcamx_dr[1] +
                                                      pt_cam.y*d_ptcamy_dr[1] +
                                                      pt_cam.z*d_ptcamz_dr[1] ),
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M *
                                         SCALE_ROTATION_CAMERA*
                                         dist_recip*( pt_cam.x*d_ptcamx_dr[2] +
                                                      pt_cam.y*d_ptcamy_dr[2] +
                                                      pt_cam.z*d_ptcamz_dr[2] ) );
                        STORE_JACOBIAN3( i_var_camera_rt + 3,
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_TRANSLATION_CAMERA*
                                         dist_recip*pt_cam.x,
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_TRANSLATION_CAMERA*
                                         dist_recip*pt_cam.y,
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_TRANSLATION_CAMERA*
                                         dist_recip*pt_cam.z );
                    }

                    if( ctx->problem_details.do_optimize_frames )
                        STORE_JACOBIAN3( i_var_point,
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_POSITION_POINT*
                                         dist_recip*(pt_cam.x*_Rc[0] + pt_cam.y*_Rc[3] + pt_cam.z*_Rc[6]),
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_POSITION_POINT*
                                         dist_recip*(pt_cam.x*_Rc[1] + pt_cam.y*_Rc[4] + pt_cam.z*_Rc[7]),
                                         DISTANCE_ERROR_EQUIVALENT__PIXELS_PER_M*
                                         SCALE_POSITION_POINT*
                                         dist_recip*(pt_cam.x*_Rc[2] + pt_cam.y*_Rc[5] + pt_cam.z*_Rc[8]) );
                    iMeasurement++;
                }
            }
        }
        else
        {
            // This is arbitrary. I'm skipping this observation, so I
            // don't touch the projection results. I need to have SOME
            // dependency on the point parameters to ensure a full-rank
            // Hessian. So if we're skipping all observations for this
            // point, I replace this cost contribution with an L2 cost
            // on the point parameters.
            for( int i_xy=0; i_xy<2; i_xy++ )
            {
                const double err = 0.0;

                if(Jt) Jrowptr[iMeasurement] = iJacobian;
                x[iMeasurement] = err;
                norm2_error += err*err;

                if( ctx->problem_details.do_optimize_intrinsic_core )
                    for(int i=0; i<4; i++)
                        STORE_JACOBIAN( i_var_intrinsics + i,
                                        0.0 );

                if( ctx->problem_details.do_optimize_intrinsic_distortions )
                    for(int i=0; i<ctx->Nintrinsics-Ncore; i++)
                        STORE_JACOBIAN( i_var_intrinsics+Ncore + i,
                                        0.0 );

                if( ctx->problem_details.do_optimize_extrinsics )
                    if( i_camera != 0 )
                    {
                        STORE_JACOBIAN3( i_var_camera_rt + 0, 0.0, 0.0, 0.0);
                        STORE_JACOBIAN3( i_var_camera_rt + 3, 0.0, 0.0, 0.0);
                    }

                if( ctx->problem_details.do_optimize_frames )
                {
                    const double dpoint = observation->skip_point ? 1.0 : 0.0;
                    // Arbitrary differences between the dimensions to keep
                    // my Hessian non-singular. This is 100% arbitrary. I'm
                    // skipping these measurements so these variables
                    // actually don't affect the computation at all
                    STORE_JACOBIAN3( i_var_point + 0, dpoint*1.1, dpoint*1.2, dpoint*1.3);
                }

                iMeasurement++;
            }

            // Now handle the reference distance, if given
            if( observation->dist > 0.0)
            {
                const double err = 0.0;

                if(Jt) Jrowptr[iMeasurement] = iJacobian;
                x[iMeasurement] = err;
                norm2_error += err*err;

                if( ctx->problem_details.do_optimize_extrinsics )
                    if(i_camera != 0)
                    {
                        STORE_JACOBIAN3( i_var_camera_rt + 0, 0.0, 0.0, 0.0);
                        STORE_JACOBIAN3( i_var_camera_rt + 3, 0.0, 0.0, 0.0);
                    }
                if( ctx->problem_details.do_optimize_frames )
                    STORE_JACOBIAN3( i_var_point, 0.0, 0.0, 0.0);
                iMeasurement++;
            }
        }
    }

    // regularization terms for the intrinsics. I favor smaller distortion
    // parameters
    if(!ctx->problem_details.do_skip_regularization &&
       ( ctx->problem_details.do_optimize_intrinsic_distortions ||
         ctx->problem_details.do_optimize_intrinsic_core
         ))
    {
        // I want the total regularization cost to be low relative to the
        // other contributions to the cost. And I want each set of
        // regularization terms to weigh roughly the same. Let's say I want
        // regularization to account for ~ .5% of the other error
        // contributions:
        //
        //   Nmeasurements_rest*normal_pixel_error_sq * 0.005/2. =
        //   Nmeasurements_regularization_distortion *normal_regularization_distortion_error_sq  =
        //   Nmeasurements_regularization_centerpixel*normal_regularization_centerpixel_error_sq =


        const bool dump_regularizaton_details = false;


        int    Nmeasurements_regularization_distortion  = ctx->Ncameras*(ctx->Nintrinsics-Ncore);
        int    Nmeasurements_regularization_centerpixel = ctx->Ncameras*2;

        int    Nmeasurements_nonregularization =
            ctx->Nmeasurements -
            Nmeasurements_regularization_distortion -
            Nmeasurements_regularization_centerpixel;

        double normal_pixel_error = 1.0;
        double expected_total_pixel_error_sq =
            (double)Nmeasurements_nonregularization *
            normal_pixel_error *
            normal_pixel_error;
        if(dump_regularizaton_details)
            MSG("expected_total_pixel_error_sq: %f", expected_total_pixel_error_sq);

        double scale_regularization_distortion =
            ({
                double normal_distortion_value = 0.2;

                double expected_regularization_distortion_error_sq_noscale =
                    (double)Nmeasurements_regularization_distortion *
                    normal_distortion_value;

                double scale_sq =
                    expected_total_pixel_error_sq * 0.005/2. / expected_regularization_distortion_error_sq_noscale;

                if(dump_regularizaton_details)
                    MSG("expected_regularization_distortion_error_sq: %f", expected_regularization_distortion_error_sq_noscale*scale_sq);

                sqrt(scale_sq);
            });
        double scale_regularization_centerpixel =
            ({

                double normal_centerpixel_offset = 50.0;

                double expected_regularization_centerpixel_error_sq_noscale =
                    (double)Nmeasurements_regularization_centerpixel *
                    normal_centerpixel_offset *
                    normal_centerpixel_offset;

                double scale_sq =
                    expected_total_pixel_error_sq * 0.005/2. / expected_regularization_centerpixel_error_sq_noscale;

                if(dump_regularizaton_details)
                    MSG("expected_regularization_centerpixel_error_sq: %f", expected_regularization_centerpixel_error_sq_noscale*scale_sq);

                sqrt(scale_sq);
            });

        for(int i_camera=0; i_camera<ctx->Ncameras; i_camera++)
        {
            const int i_var_intrinsics =
                mrcal_state_index_intrinsics(i_camera, ctx->problem_details, ctx->lensmodel);

            if( ctx->problem_details.do_optimize_intrinsic_distortions)
            {
                for(int j=0; j<ctx->Nintrinsics-Ncore; j++)
                {
                    if(Jt) Jrowptr[iMeasurement] = iJacobian;

                    // This maybe should live elsewhere, but I put it here
                    // for now. Various distortion coefficients have
                    // different meanings, and should be regularized in
                    // different ways. Specific logic follows
                    double scale = scale_regularization_distortion;

                    if( LENSMODEL_IS_OPENCV(ctx->lensmodel.type) &&
                        ctx->lensmodel.type >= LENSMODEL_OPENCV8 &&
                        5 <= j && j <= 7 )
                    {
                        // The radial distortion in opencv is x_distorted =
                        // x*scale where r2 = norm2(xy - xyc) and
                        //
                        // scale = (1 + k0 r2 + k1 r4 + k4 r6)/(1 + k5 r2 + k6 r4 + k7 r6)
                        //
                        // Note that k2,k3 are tangential (NOT radial)
                        // distortion components. Note that the r6 factor in
                        // the numerator is only present for
                        // >=LENSMODEL_OPENCV5. Note that the denominator
                        // is only present for >= LENSMODEL_OPENCV8. The
                        // danger with a rational model is that it's
                        // possible to get into a situation where scale ~
                        // 0/0 ~ 1. This would have very poorly behaved
                        // derivatives. If all the rational coefficients are
                        // ~0, then the denominator is always ~1, and this
                        // problematic case can't happen. I favor that by
                        // regularizing the coefficients in the denominator
                        // more strongly
                        scale *= 5.;
                    }

                    // This exists to avoid /0 in the gradient
                    const double eps = 1e-3;

                    double sign         = copysign(1.0, intrinsics_all[i_camera][j+Ncore]);
                    double err_no_scale = sqrt(fabs(intrinsics_all[i_camera][j+Ncore]) + eps);
                    double err          = err_no_scale * scale;

                    x[iMeasurement]  = err;
                    norm2_error     += err*err;

                    STORE_JACOBIAN( i_var_intrinsics + Ncore_state + j,
                                    scale * sign * SCALE_DISTORTION / (2. * err_no_scale) );
                    iMeasurement++;
                    if(dump_regularizaton_details)
                        MSG("regularization distortion: %g; norm2: %g", err, err*err);

                }
            }

            if( ctx->problem_details.do_optimize_intrinsic_core)
            {
                // And another regularization term: optical center should be
                // near the middle. This breaks the symmetry between moving the
                // center pixel coords and pitching/yawing the camera.
                double cx_target = 0.5 * (double)(ctx->imagersizes[i_camera*2 + 0] - 1);
                double cy_target = 0.5 * (double)(ctx->imagersizes[i_camera*2 + 1] - 1);

                double err = scale_regularization_centerpixel *
                    (intrinsics_all[i_camera][2] - cx_target);
                x[iMeasurement]  = err;
                norm2_error     += err*err;
                if(Jt) Jrowptr[iMeasurement] = iJacobian;
                STORE_JACOBIAN( i_var_intrinsics + 2,
                                scale_regularization_centerpixel * SCALE_INTRINSICS_CENTER_PIXEL );
                iMeasurement++;
                if(dump_regularizaton_details)
                    MSG("regularization center pixel off-center: %g; norm2: %g", err, err*err);

                err = scale_regularization_centerpixel *
                    (intrinsics_all[i_camera][3] - cy_target);
                x[iMeasurement]  = err;
                norm2_error     += err*err;
                if(Jt) Jrowptr[iMeasurement] = iJacobian;
                STORE_JACOBIAN( i_var_intrinsics + 3,
                                scale_regularization_centerpixel * SCALE_INTRINSICS_CENTER_PIXEL );
                iMeasurement++;
                if(dump_regularizaton_details)
                    MSG("regularization center pixel off-center: %g; norm2: %g", err, err*err);
            }
        }
    }


    // required to indicate the end of the jacobian matrix
    if( !ctx->reportFitMsg )
    {
        if(Jt) Jrowptr[iMeasurement] = iJacobian;
        assert(iMeasurement == ctx->Nmeasurements);
        assert(iJacobian    == ctx->N_j_nonzero  );

        // MSG_IF_VERBOSE("RMS: %g", sqrt(norm2_error / ((double)ctx>Nmeasurements / 2.0)));
    }
}

void mrcal_optimizerCallback(// output measurements
                             double*         x,
                             // output Jacobian. May be NULL if we don't need it
                             cholmod_sparse* Jt,

                             // in
                             // intrinsics is a concatenation of the intrinsics core
                             // and the distortion params. The specific distortion
                             // parameters may vary, depending on lensmodel, so
                             // this is a variable-length structure
                             const double*       intrinsics, // Ncameras * NlensParams
                             const pose_t*       extrinsics, // Ncameras-1 of these. Transform FROM camera0 frame
                             const pose_t*       frames,     // Nframes of these.    Transform TO   camera0 frame
                             const point3_t*     points,     // Npoints of these.    In the camera0 frame
                             const point2_t*     calobject_warp, // 1 of these. May be NULL if !problem_details.do_optimize_calobject_warp

                             int Ncameras, int Nframes, int Npoints,

                             const observation_board_t* observations_board,
                             int NobservationsBoard,

                             const observation_point_t* observations_point,
                             int NobservationsPoint,

                             int Noutlier_indices_input,
                             const int* outlier_indices_input,
                             const double* roi,
                             bool verbose,

                             lensmodel_t lensmodel,
                             const int* imagersizes, // Ncameras*2 of these

                             mrcal_problem_details_t problem_details,

                             double calibration_object_spacing,
                             int calibration_object_width_n,

                             int Nintrinsics, int Nmeasurements, int N_j_nonzero)
{
    if( calobject_warp == NULL && problem_details.do_optimize_calobject_warp )
    {
        MSG("ERROR: We're optimizing the calibration object warp, so a buffer with a seed MUST be passed in.");
        return;
    }

    if(!mrcal_modelHasCore_fxfycxcy(lensmodel))
        problem_details.do_optimize_intrinsic_core = false;

    if(!problem_details.do_optimize_intrinsic_core        &&
       !problem_details.do_optimize_intrinsic_distortions &&
       !problem_details.do_optimize_extrinsics            &&
       !problem_details.do_optimize_frames                &&
       !problem_details.do_optimize_calobject_warp)
    {
        MSG("Warning: Not optimizing any of our variables");
        return;
    }

    const int Npoints_fromBoards =
        NobservationsBoard *
        calibration_object_width_n*calibration_object_width_n;

#warning "outliers only work with board observations for now. I assume consecutive xy measurements, but points can have xyr sprinkled in there. I should make the range-full points always follow the range-less points. Then this will work"
    struct dogleg_outliers_t* markedOutliers = malloc(Npoints_fromBoards*sizeof(struct dogleg_outliers_t));
    if(markedOutliers == NULL)
    {
        MSG("Failed to allocate markedOutliers!");
        return;
    }
    memset(markedOutliers, 0, Npoints_fromBoards*sizeof(markedOutliers[0]));

    const callback_context_t ctx = {
        .intrinsics                 = intrinsics,
        .extrinsics                 = extrinsics,
        .frames                     = frames,
        .points                     = points,
        .calobject_warp             = calobject_warp,
        .Ncameras                   = Ncameras,
        .Nframes                    = Nframes,
        .Npoints                    = Npoints,
        .observations_board         = observations_board,
        .NobservationsBoard         = NobservationsBoard,
        .observations_point         = observations_point,
        .NobservationsPoint         = NobservationsPoint,
        .verbose                    = verbose,
        .lensmodel                  = lensmodel,
        .imagersizes                = imagersizes,
        .problem_details            = problem_details,
        .calibration_object_spacing = calibration_object_spacing,
        .calibration_object_width_n = calibration_object_width_n,
        .roi                        = roi,
        .Nmeasurements              = Nmeasurements,
        .N_j_nonzero                = N_j_nonzero,
        .Nintrinsics                = Nintrinsics,
        .markedOutliers             = markedOutliers};

    const int Nstate = mrcal_getNstate(Ncameras, Nframes, Npoints,
                                       problem_details,
                                       lensmodel);
    double packed_state[Nstate];
    pack_solver_state(packed_state,
                      intrinsics,
                      lensmodel,
                      extrinsics,
                      frames,
                      points,
                      calobject_warp,
                      problem_details,
                      Ncameras, Nframes, Npoints, Nstate);

    double norm2_error = -1.0;
    for(int i=0; i<Noutlier_indices_input; i++)
        markedOutliers[outlier_indices_input[i]].marked = true;

    optimizerCallback(packed_state, x, Jt, &ctx);
    free(markedOutliers);
}

mrcal_stats_t
mrcal_optimize( // out
                // These may be NULL. They're for diagnostic reporting to the
                // caller
                double* x_final,
                double* covariance_intrinsics_full,
                double* covariance_intrinsics,
                double* covariance_extrinsics,

                // Buffer should be at least Npoints long. stats->Noutliers
                // elements will be filled in
                int*    outlier_indices_final,
                // Buffer should be at least Npoints long. stats->NoutsideROI
                // elements will be filled in
                int*    outside_ROI_indices_final,

                // out, in

                // if(_solver_context != NULL) then this is a persistent solver
                // context. The context is NOT freed on exit.
                // mrcal_free_context() should be called to release it
                //
                // if(*_solver_context != NULL), the given context is reused
                // if(*_solver_context == NULL), a context is created, and
                // returned here on exit
                void** _solver_context,

                // These are a seed on input, solution on output
                // These are the state. I don't have a state_t because Ncameras
                // and Nframes aren't known at compile time
                //
                // intrinsics is a concatenation of the intrinsics core
                // and the distortion params. The specific distortion
                // parameters may vary, depending on lensmodel, so
                // this is a variable-length structure
                double*       intrinsics, // Ncameras * NlensParams of these
                pose_t*       extrinsics, // Ncameras-1 of these. Transform FROM camera0 frame
                pose_t*       frames,     // Nframes of these.    Transform TO   camera0 frame
                point3_t*     points,     // Npoints of these.    In the camera0 frame
                point2_t*     calobject_warp, // 1 of these. May be NULL if !problem_details.do_optimize_calobject_warp

                // in
                int Ncameras, int Nframes, int Npoints,

                const observation_board_t* observations_board,
                int NobservationsBoard,

                const observation_point_t* observations_point,
                int NobservationsPoint,

                bool check_gradient,
                // input outliers. These are respected regardless of
                // skip_outlier_rejection.
                int Noutlier_indices_input,
                int* outlier_indices_input,
                const double* roi,
                bool verbose,
                // Whether to try to find NEW outliers. These would be added to
                // the outlier_indices_input, which are respected regardless
                const bool skip_outlier_rejection,

                lensmodel_t lensmodel,
                double observed_pixel_uncertainty,
                const int* imagersizes, // Ncameras*2 of these

                mrcal_problem_details_t problem_details,

                double calibration_object_spacing,
                int calibration_object_width_n)
{
    if( calobject_warp == NULL && problem_details.do_optimize_calobject_warp )
    {
        MSG("ERROR: We're optimizing the calibration object warp, so a buffer with a seed MUST be passed in.");
        return (mrcal_stats_t){.rms_reproj_error__pixels = -1.0};
    }

    if(!mrcal_modelHasCore_fxfycxcy(lensmodel))
        problem_details.do_optimize_intrinsic_core = false;

    if(!problem_details.do_optimize_intrinsic_core        &&
       !problem_details.do_optimize_intrinsic_distortions &&
       !problem_details.do_optimize_extrinsics            &&
       !problem_details.do_optimize_frames                &&
       !problem_details.do_optimize_calobject_warp)
    {
        MSG("Warning: Not optimizing any of our variables");
    }

    dogleg_parameters2_t dogleg_parameters;
    dogleg_getDefaultParameters(&dogleg_parameters);
    dogleg_parameters.dogleg_debug = verbose ? DOGLEG_DEBUG_VNLOG : 0;

#warning update these parameters
    // These were derived empirically, seeking high accuracy, fast convergence
    // and without serious concern for performance. I looked only at a single
    // frame. Tweak them please
    dogleg_parameters.Jt_x_threshold = 0;
    dogleg_parameters.update_threshold = 1e-6;
    dogleg_parameters.trustregion_threshold = 0;
    dogleg_parameters.max_iterations = 300;
    // dogleg_parameters.trustregion_decrease_factor    = 0.1;
    // dogleg_parameters.trustregion_decrease_threshold = 0.15;
    // dogleg_parameters.trustregion_increase_factor    = 4.0
    // dogleg_parameters.trustregion_increase_threshold = 0.75;

    const int Npoints_fromBoards =
        NobservationsBoard *
        calibration_object_width_n*calibration_object_width_n;

#warning "outliers only work with board observations for now. I assume consecutive xy measurements, but points can have xyr sprinkled in there. I should make the range-full points always follow the range-less points. Then this will work"
    struct dogleg_outliers_t* markedOutliers = malloc(Npoints_fromBoards*sizeof(struct dogleg_outliers_t));
    if(markedOutliers == NULL)
    {
        MSG("Failed to allocate markedOutliers!");
        return (mrcal_stats_t){.rms_reproj_error__pixels = -1.0};
    }
    memset(markedOutliers, 0, Npoints_fromBoards*sizeof(markedOutliers[0]));

    callback_context_t ctx = {
        .intrinsics                 = intrinsics,
        .extrinsics                 = extrinsics,
        .frames                     = frames,
        .points                     = points,
        .calobject_warp             = calobject_warp,
        .Ncameras                   = Ncameras,
        .Nframes                    = Nframes,
        .Npoints                    = Npoints,
        .observations_board         = observations_board,
        .NobservationsBoard         = NobservationsBoard,
        .observations_point         = observations_point,
        .NobservationsPoint         = NobservationsPoint,
        .verbose                    = verbose,
        .lensmodel                  = lensmodel,
        .imagersizes                = imagersizes,
        .problem_details            = problem_details,
        .calibration_object_spacing = calibration_object_spacing,
        .calibration_object_width_n = calibration_object_width_n,
        .roi                        = roi,
        .Nmeasurements              = mrcal_getNmeasurements_all(Ncameras, NobservationsBoard,
                                                                 observations_point, NobservationsPoint,
                                                                 calibration_object_width_n,
                                                                 problem_details,
                                                                 lensmodel),
        .N_j_nonzero                = mrcal_getN_j_nonzero(Ncameras,
                                                           observations_board, NobservationsBoard,
                                                           observations_point, NobservationsPoint,
                                                           problem_details,
                                                           lensmodel,
                                                           calibration_object_width_n),
        .Nintrinsics                = mrcal_getNlensParams(lensmodel),

        .markedOutliers = markedOutliers};


    dogleg_solverContext_t*  solver_context = NULL;
    // If I have a context already, I free it and create it anew later. Ideally
    // I'd reuse it, but then I'd need to make sure it's valid and such. Too
    // much work for now
    if(_solver_context != NULL && *_solver_context != NULL)
        dogleg_freeContext((dogleg_solverContext_t**)_solver_context);

    const int Nstate = mrcal_getNstate(Ncameras, Nframes, Npoints,
                                       problem_details,
                                       lensmodel);
    double packed_state[Nstate];
    pack_solver_state(packed_state,
                      intrinsics,
                      lensmodel,
                      extrinsics,
                      frames,
                      points,
                      calobject_warp,
                      problem_details,
                      Ncameras, Nframes, Npoints, Nstate);

    double norm2_error = -1.0;
    mrcal_stats_t stats = {.rms_reproj_error__pixels = -1.0 };

    if( !check_gradient )
    {
        stats.Noutliers = 0;
        for(int i=0; i<Noutlier_indices_input; i++)
        {
            markedOutliers[outlier_indices_input[i]].marked = true;
            stats.Noutliers++;
        }

        if(verbose)
        {
            ctx.reportFitMsg = "Before";
#warning hook this up
            //        optimizerCallback(packed_state, NULL, NULL, &ctx);
        }
        ctx.reportFitMsg = NULL;


        double outliernessScale = -1.0;
        do
        {
            norm2_error = dogleg_optimize2(packed_state,
                                           Nstate, ctx.Nmeasurements, ctx.N_j_nonzero,
                                           (dogleg_callback_t*)&optimizerCallback, &ctx,
                                           &dogleg_parameters,
                                           &solver_context);
            if(_solver_context != NULL)
                *_solver_context = solver_context;

            if(norm2_error < 0)
                // libdogleg barfed. I quit out
                goto done;

#if 0
            // Not using dogleg_markOutliers() (for now?)

            if(outliernessScale < 0.0 && verbose)
                // These are for debug reporting
                dogleg_reportOutliers(getConfidence,
                                      &outliernessScale,
                                      2, Npoints_fromBoards,
                                      stats.Noutliers,
                                      solver_context->beforeStep, solver_context);
#endif

        } while( !skip_outlier_rejection &&
                 markOutliers(markedOutliers,
                              &stats.Noutliers,
                              observations_board,
                              NobservationsBoard,
                              calibration_object_width_n,
                              roi,
                              solver_context->beforeStep->x,
                              observed_pixel_uncertainty,
                              verbose) );

        // Done. I have the final state. I spit it back out
        unpack_solver_state( intrinsics, // Ncameras of these
                             extrinsics, // Ncameras-1 of these
                             frames,     // Nframes of these
                             points,     // Npoints of these
                             calobject_warp,
                             packed_state,
                             lensmodel,
                             problem_details,
                             Ncameras, Nframes, Npoints, Nstate);
        if(verbose)
        {
            // Not using dogleg_markOutliers() (for now?)
#if 0
            // These are for debug reporting
            dogleg_reportOutliers(getConfidence,
                                  &outliernessScale,
                                  2, Npoints_fromBoards,
                                  stats.Noutliers,
                                  solver_context->beforeStep, solver_context);
#endif

            ctx.reportFitMsg = "After";
#warning hook this up
            //        optimizerCallback(packed_state, NULL, NULL, &ctx);
        }

        if(!problem_details.do_skip_regularization)
        {
            double norm2_err_regularization = 0;
            int    Nmeasurements_regularization =
                Ncameras*getNregularizationTerms_percamera(problem_details,
                                                           lensmodel);

            for(int i=0; i<Nmeasurements_regularization; i++)
            {
                double x = solver_context->beforeStep->x[ctx.Nmeasurements - Nmeasurements_regularization + i];
                norm2_err_regularization += x*x;
            }

            double norm2_err_nonregularization = norm2_error - norm2_err_regularization;
            double ratio_regularization_cost = norm2_err_regularization / norm2_err_nonregularization;

            if(verbose)
            {
                for(int i=0; i<Nmeasurements_regularization; i++)
                {
                    double x = solver_context->beforeStep->x[ctx.Nmeasurements - Nmeasurements_regularization + i];
                    MSG("regularization %d: %f (squared: %f)", i, x, x*x);
                }
                MSG("norm2_error: %f", norm2_error);
                MSG("norm2_err_regularization: %f", norm2_err_regularization);
                MSG("regularization cost ratio: %g", ratio_regularization_cost);
            }
        }
    }
    else
        for(int ivar=0; ivar<Nstate; ivar++)
            dogleg_testGradient(ivar, packed_state,
                                Nstate, ctx.Nmeasurements, ctx.N_j_nonzero,
                                (dogleg_callback_t*)&optimizerCallback, &ctx);

    stats.rms_reproj_error__pixels =
        // /2 because I have separate x and y measurements
        sqrt(norm2_error / ((double)ctx.Nmeasurements / 2.0));

    if(x_final)
        memcpy(x_final, solver_context->beforeStep->x, ctx.Nmeasurements*sizeof(double));

    if( covariance_intrinsics_full ||
        covariance_intrinsics ||
        covariance_extrinsics )
    {
        int Nintrinsics_per_camera = mrcal_getNlensParams(lensmodel);
        bool result =
            computeUncertaintyMatrices(// out
                                       // dimensions (Ncameras,Nintrinsics_per_camera,Nintrinsics_per_camera)
                                       covariance_intrinsics_full,
                                       covariance_intrinsics,
                                       // dimensions ((Ncameras-1)*6,(Ncameras-1)*6)
                                       covariance_extrinsics,
                                       observed_pixel_uncertainty,

                                       // in
                                       lensmodel,
                                       problem_details,
                                       Ncameras,
                                       NobservationsBoard,
                                       NobservationsPoint,
                                       Nframes, Npoints,
                                       calibration_object_width_n,

                                       solver_context);
        if(!result)
        {
            MSG("Failed to compute covariance_...");
            double nan = strtod("NAN", NULL);
            if(covariance_intrinsics_full)
                for(int i=0; i<Ncameras*Nintrinsics_per_camera*Nintrinsics_per_camera; i++)
                {
                    covariance_intrinsics_full[i] = nan;
                    covariance_intrinsics[i]      = nan;
                }
            if(covariance_extrinsics)
                for(int i=0; i<Ncameras*6 * Ncameras*6; i++)
                    covariance_extrinsics[i] = nan;
        }
    }
    if(outlier_indices_final)
    {
        int ioutlier = 0;
        for(int iFeature=0; iFeature<Npoints_fromBoards; iFeature++)
            if( markedOutliers[iFeature].marked )
                outlier_indices_final[ioutlier++] = iFeature;

        assert(ioutlier == stats.Noutliers);
    }
    if(outside_ROI_indices_final)
    {
        stats.NoutsideROI = 0;
        if( roi != NULL )
        {
            for(int i_observation_board=0;
                i_observation_board<NobservationsBoard;
                i_observation_board++)
            {
                const observation_board_t* observation = &observations_board[i_observation_board];
                const int i_camera = observation->i_camera;
                for(int i_pt=0;
                    i_pt < calibration_object_width_n*calibration_object_width_n;
                    i_pt++)
                {
                    const point3_t* pt_observed = &observation->px[i_pt];
                    double weight = region_of_interest_weight(pt_observed, roi, i_camera);
                    if( weight != 1.0 )
                        outside_ROI_indices_final[stats.NoutsideROI++] =
                            i_observation_board*calibration_object_width_n*calibration_object_width_n +
                            i_pt;
                }
            }
        }
    }

 done:
    if(_solver_context == NULL && solver_context)
        dogleg_freeContext(&solver_context);

    free(markedOutliers);
    return stats;
}

// frees a dogleg_solverContext_t. I don't want to #include <dogleg.h> here, so
// this is void
void mrcal_free_context(void** ctx)
{
    if( *ctx == NULL )
        return;

    dogleg_freeContext((dogleg_solverContext_t**)ctx);
}
