/* -------------------------------------------------------------------------- *
 *                               OpenMM-AGBNP                                *
 * -------------------------------------------------------------------------- */

#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cmath>
#include <cfloat>
//#include "openmm/reference/SimTKOpenMMRealType.h"
#include "AGBNPUtils.h"
#include "ReferenceAGBNPKernels.h"
#include "AGBNPForce.h"
#include "openmm/OpenMMException.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/internal/SplineFitter.h"
#include "openmm/reference/RealVec.h"
#include "openmm/reference/ReferencePlatform.h"
#include "gaussvol.h"


using namespace AGBNPPlugin;
using namespace OpenMM;
using namespace std;


// ---- Lifted from gaussvol.cpp, need to share somehow ----

/* overlap volume switching function + 1st derivative */
static RealOpenMM pol_switchfunc(RealOpenMM gvol, RealOpenMM volmina, RealOpenMM volminb, RealOpenMM &sp){

  RealOpenMM swf = 0.0f;
  RealOpenMM swfp = 1.0f;
  RealOpenMM swd, swu, swu2, swu3, s;
  if(gvol > volminb) {
    swf = 1.0f;
    swfp = 0.0f;
  }else if(gvol < volmina){
    swf = 0.0f;
    swfp = 0.0f;
  }
  swd = 1.f/(volminb - volmina);
  swu = (gvol - volmina)*swd;
  swu2 = swu*swu;
  swu3 = swu*swu2;
  s = swf + swfp*swu3*(10.f-15.f*swu+6.f*swu2);
  sp = swfp*swd*30.f*swu2*(1.f - 2.f*swu + swu2);

  //turn off switching function
  //*sp = 0.0;
  //s = 1.0;
  return s;
}

/* overlap between two Gaussians represented by a (V,c,a) triplet
   V: volume of Gaussian
   c: position of Gaussian
   a: exponential coefficient

   g(x) = V (a/pi)^(3/2) exp(-a(x-c)^2)

   this version is based on V=V(V1,V2,r1,r2,alpha)
   alpha = (a1 + a2)/(a1 a2)

   dVdr is (1/r)*(dV12/dr)
   dVdV is dV12/dV1 
   dVdalpha is dV12/dalpha
   d2Vdalphadr is (1/r)*d^2V12/dalpha dr
   d2VdVdr is (1/r) d^2V12/dV1 dr

*/
static RealOpenMM ogauss_alpha(GaussianVca &g1, GaussianVca &g2, GaussianVca &g12, RealOpenMM &dVdr, RealOpenMM &dVdV, RealOpenMM &sfp){
  RealOpenMM d2, dx, dy, dz;
  RealVec c1 = g1.c;
  RealVec c2 = g2.c;
  RealVec dist;
  RealOpenMM deltai, gvol, p12, a12;
  RealOpenMM s, sp, df, dgvol, dgvolv, ef, dgvola2, dgvola1, dgalpha, dgalpha2, dgvolvdr;

  dist = c2 - c1;
  d2 = dist.dot(dist);

  a12 = g1.a + g2.a;
  deltai = 1./a12;
  df = (g1.a)*(g2.a)*deltai; // 1/alpha

  ef = exp(-df*d2);
  gvol = ( (g1.v * g2.v)/pow(PI/df,1.5))*ef;
  dgvol = -2.f*df*gvol; // (1/r)*(dV/dr) w/o switching function
  dgvolv = gvol/g1.v;     // (dV/dV1)  w/o switching function

  /* parameters for overlap gaussian. Note that c1 and c2 are Vec3's and the "*" operator wants 
     the vector first and scalar second vector2 = vector1 * scalar */
  g12.c = ((c1 * g1.a) + (c2 * g2.a)) * deltai;
  g12.a = a12;
  g12.v = gvol;

  /* switching function */
  s = pol_switchfunc(gvol, VOLMINA, VOLMINB, sp);
  sfp = sp*gvol+s;
  dVdr = dgvol;
  dVdV = dgvolv;

  return s*gvol;
}
// ---- end of "Lifted from gaussvol.cpp" --------------------


static vector<RealVec>& extractPositions(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *((vector<RealVec>*) data->positions);
}

static vector<RealVec>& extractForces(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *((vector<RealVec>*) data->forces);
}


/* a switching function for the inverse born radius (beta)
   so that if beta is negative -> beta' = minbeta
*/ 
 static RealOpenMM agbnp_swf_invbr(RealOpenMM beta, RealOpenMM& fp){
  /* the maximum born radius is max reach of Q4 lookup table */
  static const RealOpenMM  a  = 1./AGBNP_I4LOOKUP_MAXA;
  static const RealOpenMM  a2 = 1./(AGBNP_I4LOOKUP_MAXA*AGBNP_I4LOOKUP_MAXA);

  RealOpenMM t;
  if(beta<0.0){
    t = a;
    fp = 0.0;
  }else{
    t = sqrt(a2 + beta*beta);
    fp  = beta/t;
  }
  return t;
}

// Initializes AGBNP library
void ReferenceCalcAGBNPForceKernel::initialize(const System& system, const AGBNPForce& force) {
   

    numParticles = force.getNumParticles();

    //set version
    version = force.getVersion();
    
    //input lists
    positions.resize(numParticles);
    radii_large.resize(numParticles);//van der Waals radii + offset (large radii)
    radii_vdw.resize(numParticles);//van der Waals radii (small radii)
    gammas.resize(numParticles);
    vdw_alpha.resize(numParticles);
    charge.resize(numParticles);
    ishydrogen.resize(numParticles);

    //output lists
    free_volume.resize(numParticles);
    self_volume.resize(numParticles);
    surface_areas.resize(numParticles);
    vol_force.resize(numParticles);
    
    vector<double> vdwrad(numParticles);
    double common_gamma = -1;
    for (int i = 0; i < numParticles; i++){
      double r, g, alpha, q;
      bool h;
      force.getParticleParameters(i, r, g, alpha, q, h);
      radii_large[i] = r + AGBNP_RADIUS_INCREMENT;
      radii_vdw[i] = r;
      vdwrad[i] = r; //double version for lookup table setup
      gammas[i] = g;
      if(h) gammas[i] = 0.0;
      vdw_alpha[i] = alpha;
      charge[i] = q;
      ishydrogen[i] = h ? 1 : 0;

      //make sure that all gamma's are the same
      if(common_gamma < 0 && !h){
	common_gamma = g; //first occurrence of a non-zero gamma
      }else{
	if(!h && pow(common_gamma - g,2) > FLT_MIN){
	  throw OpenMMException("initialize(): AGBNP does not support multiple gamma values.");
	}
      }

    }

    //create and saves GaussVol instance
    //loads some initial values for radii and gamma but they will be
    //reset in execute()
    gvol = new GaussVol(numParticles, ishydrogen);

    //initializes I4 lookup table for Born-radii calculation
    double rmin = 0.;
    double rmax = AGBNP_I4LOOKUP_MAXA;
    int i4size = AGBNP_I4LOOKUP_NA;
    i4_lut = new AGBNPI42DLookupTable(vdwrad, ishydrogen, i4size, rmin, rmax, version);

    //volume scaling factors and born radii
    volume_scaling_factor.resize(numParticles);
    inverse_born_radius.resize(numParticles);
    inverse_born_radius_fp.resize(numParticles);
    born_radius.resize(numParticles);

    solvent_radius = force.getSolventRadius();
}

double ReferenceCalcAGBNPForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy) {
  double energy = 0.0;
  if(version == 0){
    energy = executeGVolSA(context, includeForces, includeEnergy);
  }else if(version == 1){
    energy = executeAGBNP1(context, includeForces, includeEnergy);
  }else if(version == 2){
    energy = executeAGBNP2(context, includeForces, includeEnergy);
  }
  return energy;
}


double ReferenceCalcAGBNPForceKernel::executeGVolSA(ContextImpl& context, bool includeForces, bool includeEnergy) {

  //sequence: volume1->volume2

  
  //weights
    RealOpenMM w_evol = 1.0;
    
    vector<RealVec>& pos = extractPositions(context);
    vector<RealVec>& force = extractForces(context);
    RealOpenMM energy = 0.0;
    int verbose_level = 0;
    int init = 0; 

    vector<RealOpenMM> nu(numParticles);


    if(verbose_level > 0) cout << "Executing GVolSA" << endl;
    
    if(verbose_level > 0){
      cout << "-----------------------------------------------" << endl;
    } 

    
    // volume energy function 1
    RealOpenMM volume1, vol_energy1;
    for(int i = 0; i < numParticles; i++){
      nu[i] = gammas[i]/SA_DR;
    }
    vector<RealOpenMM> volumes_large(numParticles);
    for(int i = 0; i < numParticles; i++){
      volumes_large[i] = 4.*M_PI*pow(radii_large[i],3)/3.;
    }
    gvol->compute_tree(pos, radii_large, volumes_large, nu);
    gvol->compute_volume(pos, volume1, vol_energy1, vol_force, free_volume, self_volume);
      
    //returns energy and gradients from volume energy function
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_evol;
    }
    energy += vol_energy1 * w_evol;
    if(verbose_level > 0){
      cout << "Volume energy 1: " << vol_energy1 << endl;
    }

    // volume energy function 2 (small radii)
    RealOpenMM vol_energy2, volume2;
    for(int i = 0; i < numParticles; i++){
      nu[i] = -gammas[i]/SA_DR;
    }
    vector<RealOpenMM> volumes_vdw(numParticles);
    for(int i = 0; i < numParticles; i++){
      volumes_vdw[i] = 4.*M_PI*pow(radii_vdw[i],3)/3.;
    }
    gvol->rescan_tree_volumes(pos, radii_vdw, volumes_vdw, nu);
    gvol->compute_volume(pos, volume2, vol_energy2, vol_force, free_volume, self_volume);
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_evol;
    }
    energy += vol_energy2 * w_evol;
    if(verbose_level > 0){
      cout << "Volume energy 2: " << vol_energy2 << endl;
      cout << "Surface area energy: " << vol_energy1 + vol_energy2 << endl;
    }
    
    //returns energy
    return (double)energy;
}


double ReferenceCalcAGBNPForceKernel::executeAGBNP1(ContextImpl& context, bool includeForces, bool includeEnergy) {
    //weights
    RealOpenMM w_evol = 1.0, w_egb = 1.0, w_vdw = 1.0;
  
    vector<RealVec>& pos = extractPositions(context);
    vector<RealVec>& force = extractForces(context);
    RealOpenMM energy = 0.0;
    int verbose_level = 0;
    bool verbose = verbose_level > 1;
    int init = 0;
    
    if(verbose_level > 0) {
      cout << "Executing AGBNP1" << endl;
      cout << "-----------------------------------------------" << endl;
    } 
    



    
    // volume energy function 1
    vector<RealOpenMM> nu(numParticles);
    RealOpenMM volume1, vol_energy1;
    for(int i = 0; i < numParticles; i++){
      nu[i] = gammas[i]/SA_DR;
    }
    vector<RealOpenMM> volumes_large(numParticles);
    for(int i = 0; i < numParticles; i++){
      volumes_large[i] = ishydrogen[i]>0 ? 0.0 : 4.*M_PI*pow(radii_large[i],3)/3.;
    }
    gvol->compute_tree(pos, radii_large, volumes_large, nu);
    if(verbose_level > 4){
      gvol->print_tree();
    }
    gvol->compute_volume(pos, volume1, vol_energy1, vol_force, free_volume, self_volume);


    if(verbose_level > 0){
      vector<int> noverlaps(numParticles);
      for(int i = 0; i<numParticles; i++) noverlaps[i] = 0;
      gvol->getstat(noverlaps);
      
      //compute maximum number of overlaps
      int nn = 0;
      for(int i = 0; i < noverlaps.size(); i++){
	nn += noverlaps[i];
      }

      cout << "Number of overlaps: " << nn << endl;
    }



    
    //returns energy and gradients from volume energy function
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_evol;
    }
    energy += vol_energy1 * w_evol;
    if(verbose_level > 0){
      cout << "Volume energy 1: " << vol_energy1 << endl;
    }

    double tot_vol = 0;
    double vol_energy = 0;
    for(int i = 0; i < numParticles; i++){
      tot_vol += self_volume[i];
      vol_energy += nu[i]*self_volume[i];
    }
    if(verbose_level > 0){
      cout << "Volume from self volumes(1): " << tot_vol << endl;
      cout << "Volume energy from self volumes(1): " << vol_energy << endl;
    }



    
    // volume energy function 2 (small radii)
    RealOpenMM vol_energy2, volume2;
    for(int i = 0; i < numParticles; i++){
      nu[i] = -gammas[i]/SA_DR;
    }
    vector<RealOpenMM> volumes_vdw(numParticles);
    for(int i = 0; i < numParticles; i++){
      volumes_vdw[i] = ishydrogen[i]>0 ? 0.0 : 4.*M_PI*pow(radii_vdw[i],3)/3.;
    }
    gvol->rescan_tree_volumes(pos, radii_vdw, volumes_vdw, nu);
    gvol->compute_volume(pos, volume2, vol_energy2, vol_force, free_volume, self_volume);
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_evol;
    }
    energy += vol_energy2 * w_evol;
    if(verbose_level > 0){
      cout << "Volume energy 2: " << vol_energy2 << endl;
      cout << "Surface area energy: " << vol_energy1 + vol_energy2 << endl;
    }
    
    //now overlap tree is set up with small radii
    
#ifdef NOTNOW
    vector<int> nov, nov_2body;
    gvol->getstat(nov, nov_2body);    
    int nn = 0, nn2 = 0;
    for(int i = 0; i < nov.size(); i++){
      nn += nov[i];
     nn2 += nov_2body[i];
    }
    cout << "Noverlaps: " << nn << " " << nn2 << endl;
#endif

#ifdef NOTNOW    
    {
      //tests i4 lookup table
      double dd = 0.12;
      double dmin = 0.;
      double dmax = AGBNP_I4LOOKUP_MAXA/0.165;
      for(int i=0;i<100;i++){
	double x = dmin + i*dd;
	double y = (x < dmax) ? i4_lut->eval(x, 1.0) : 0.;
	double yp = (x < dmax) ? i4_lut->evalderiv(x, 1.0) : 0.;
	cout << "i4: " << x << " " << y << " " << yp << endl;
      }
    }
#endif

    //volume scaling factors from self volumes (with small radii)
    tot_vol = 0;
    for(int i = 0; i < numParticles; i++){
      RealOpenMM rad = radii_vdw[i];
      RealOpenMM vol = (4./3.)*M_PI*rad*rad*rad;
      volume_scaling_factor[i] = self_volume[i]/vol;
      if(verbose_level > 3){
	cout << "SV " << i << " " << self_volume[i] << endl;
      }
      tot_vol += self_volume[i];
    }
    if(verbose_level > 0){
      cout << "Volume from self volumes: " << tot_vol << endl;
    }

    RealOpenMM pifac = 1./(4.*M_PI);

    //compute inverse Born radii, prototype, no cutoff
    for(int i = 0; i < numParticles; i++){
      inverse_born_radius[i] = 1./radii_vdw[i];
      for(int j = 0; j < numParticles; j++){
	if(i == j) continue;
	if(ishydrogen[j] > 0) continue;
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d = sqrt(dist.dot(dist));
	if(d < AGBNP_I4LOOKUP_MAXA){
	  int rad_typei = i4_lut->radius_type_screened[i];
	  int rad_typej = i4_lut->radius_type_screener[j];
	  inverse_born_radius[i] -= pifac*volume_scaling_factor[j]*i4_lut->eval(d, rad_typei, rad_typej);
	}	
      }
      RealOpenMM fp;
      born_radius[i] = 1./agbnp_swf_invbr(inverse_born_radius[i], fp);
      inverse_born_radius_fp[i] = fp;
    }

    if(verbose_level > 3){
      cout << "Born radii:" << endl;
      RealOpenMM fp;
      for(int i = 0; i < numParticles; i++){
	cout << "BR " << i << " " << 10.*born_radius[i] << " Si " << volume_scaling_factor[i] << endl;
      }
    }

    //GB energy
    RealOpenMM dielectric_in = 1.0;
    RealOpenMM dielectric_out= 80.0;
    RealOpenMM tokjmol = 4.184*332.0/10.0; //the factor of 10 is the conversion of 1/r from nm to Ang
    RealOpenMM dielectric_factor = tokjmol*(-0.5)*(1./dielectric_in - 1./dielectric_out);
    RealOpenMM pt25 = 0.25;
    vector<RealOpenMM> egb_der_Y(numParticles);
    for(int i = 0; i < numParticles; i++){
      egb_der_Y[i] = 0.0;
    }
    RealOpenMM gb_self_energy = 0.0;
    RealOpenMM gb_pair_energy = 0.0;
    for(int i = 0; i < numParticles; i++){
      double uself = dielectric_factor*charge[i]*charge[i]/born_radius[i];
      gb_self_energy += uself;
      for(int j = i+1; j < numParticles; j++){
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d2 = dist.dot(dist);
	RealOpenMM qqf = charge[j]*charge[i];
	RealOpenMM qq = dielectric_factor*qqf;
	RealOpenMM bb = born_radius[i]*born_radius[j];
	RealOpenMM etij = exp(-pt25*d2/bb);
	RealOpenMM fgb = 1./sqrt(d2 + bb*etij);
	RealOpenMM egb = 2.*qq*fgb;
	gb_pair_energy += egb;
	RealOpenMM fgb3 = fgb*fgb*fgb;
	RealOpenMM mw = -2.0*qq*(1.0-pt25*etij)*fgb3;
	RealVec g = dist * mw;
	force[i] += g * w_egb;
	force[j] -= g * w_egb;
	RealOpenMM ytij = qqf*(bb+pt25*d2)*etij*fgb3;
	egb_der_Y[i] += ytij;
	egb_der_Y[j] += ytij;
      }
    }
    if(verbose_level > 0){
      cout << "GB self energy: " << gb_self_energy << endl;
      cout << "GB pair energy: " << gb_pair_energy << endl;
      cout << "GB energy: " << gb_pair_energy+gb_self_energy << endl;
    }
    energy += w_egb*gb_pair_energy + w_egb*gb_self_energy;

    if(verbose_level > 0){
      cout << "Y parameters: " << endl;
      for(int i = 0; i < numParticles; i++){
	cout << "Y: " << i << " " << egb_der_Y[i] << endl;
      }
    }

    //compute van der Waals energy
    RealOpenMM evdw = 0.;
    for(int i=0;i<numParticles; i++){
      evdw += vdw_alpha[i]/pow(born_radius[i]+AGBNP_HB_RADIUS,3);
    }
    if(verbose_level > 0){
      cout << "Van der Waals energy: " << evdw << endl;
    }
    energy += w_vdw*evdw;

    //compute atom-level property for the calculation of the gradients of Evdw and Egb
    vector<RealOpenMM> evdw_der_brw(numParticles);
    for(int i = 0; i < numParticles; i++){
      RealOpenMM br = born_radius[i];
      evdw_der_brw[i] = -pifac*3.*vdw_alpha[i]*br*br*inverse_born_radius_fp[i]/pow(br+AGBNP_HB_RADIUS,4);
    }
    if(verbose_level > 3){
      cout << "BrW parameters: " << endl;
      for(int i = 0; i < numParticles; i++){
	cout << "BrW: " << i << " " << evdw_der_brw[i] << endl;      
      }
    }

    
    vector<RealOpenMM> egb_der_bru(numParticles);
    for(int i = 0; i < numParticles; i++){
      RealOpenMM br = born_radius[i];
      RealOpenMM qi = charge[i];
      egb_der_bru[i] = -pifac*dielectric_factor*(qi*qi + egb_der_Y[i]*br)*inverse_born_radius_fp[i];
    }
    
    if(verbose_level > 3){
      cout << "BrU parameters: " << endl;
      for(int i = 0; i < numParticles; i++){
	cout << "BrU: " << i << " " << egb_der_bru[i] << endl;      
      }
    }
    
    
    //compute the component of the gradients of the van der Waals and GB energies due to
    //variations of Born radii
    //also accumulates W's and U's for self-volume components of the gradients later
    vector<RealOpenMM> evdw_der_W(numParticles);
    vector<RealOpenMM> egb_der_U(numParticles);
    for(int i = 0; i < numParticles; i++){
      evdw_der_W[i] = egb_der_U[i] = 0.0;
    }
    for(int i = 0; i < numParticles; i++){
      for(int j = 0; j < numParticles; j++){
	if(i == j) continue;
	if(ishydrogen[j]>0) continue;
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d = sqrt(dist.dot(dist));
	double Qji = 0.0, dQji = 0.0;
	// Qji: j descreens i
	if(d < AGBNP_I4LOOKUP_MAXA){
	  int rad_typei = i4_lut->radius_type_screened[i];
	  int rad_typej = i4_lut->radius_type_screener[j];
	  Qji = i4_lut->eval(d, rad_typei, rad_typej); 
	  dQji = i4_lut->evalderiv(d, rad_typei, rad_typej); 
	}
	RealVec w;
	//van der Waals stuff
	evdw_der_W[j] += evdw_der_brw[i]*Qji;
	w = dist * evdw_der_brw[i]*volume_scaling_factor[j]*dQji/d;
	force[i] += w * w_vdw;
	force[j] -= w * w_vdw;
	//GB stuff
	egb_der_U[j] += egb_der_bru[i]*Qji;
	w = dist * egb_der_bru[i]*volume_scaling_factor[j]*dQji/d;
	force[i] += w * w_egb;
	force[j] -= w * w_egb;
      }
    }

    

    if(verbose_level > 3){
      cout << "U parameters: " << endl;
      for(int i = 0; i < numParticles; i++){
	RealOpenMM vol = 4.*M_PI*pow(radii_vdw[i],3)/3.0; 
	cout << "U: " << i << " " << egb_der_U[i]/vol << endl;      
      }
    }

    if(verbose_level > 3){
      cout << "W parameters: " << endl;
      for(int i = 0; i < numParticles; i++){
	RealOpenMM vol = 4.*M_PI*pow(radii_vdw[i],3)/3.0; 
	cout << "W: " << i << " " << evdw_der_W[i]/vol << endl;      
      }
    }


    
    

#ifdef NOTNOW
    //
    // test derivative of van der Waals energy at constant self volumes
    //
    int probe_atom = 120;
    RealVec dx = RealVec(0.001, 0.0, 0.0);
    pos[probe_atom] += dx;
    //recompute Born radii w/o changing volume scaling factors
    for(int i = 0; i < numParticles; i++){
      inverse_born_radius[i] = 1./(radii[i] - AGBNP_RADIUS_INCREMENT);
      for(int j = 0; j < numParticles; j++){
	if(i == j) continue;
	if(ishydrogen[j]>0) continue;
	RealOpenMM b = (radii[i] - AGBNP_RADIUS_INCREMENT)/radii[j];
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d = sqrt(dist.dot(dist));
	if(d < AGBNP_I4LOOKUP_MAXA){
	  int rad_typei = i4_lut->radius_type_screened[i];
	  int rad_typej = i4_lut->radius_type_screened[j];
	  inverse_born_radius[i] -= pifac*volume_scaling_factor[j]*i4_lut->eval(d, b); 
	}	
      }
      RealOpenMM fp;
      born_radius[i] = 1./agbnp_swf_invbr(inverse_born_radius[i], fp);
      inverse_born_radius_fp[i] = fp;
    }
    for(int i = 0; i < numParticles; i++){
      RealOpenMM br = born_radius[i];
      evdw_der_brw[i] = -pifac*3.*vdw_alpha[i]*br*br*inverse_born_radius_fp[i]/pow(br+AGBNP_HB_RADIUS,4);
    }
    RealOpenMM evdw_new = 0.;
    for(int i=0;i<numParticles; i++){
      evdw_new += vdw_alpha[i]/pow(born_radius[i]+AGBNP_HB_RADIUS,3);
    }
    if(verbose_level > 0){
      cout << "New Van der Waals energy: " << evdw_new << endl;
    }
    RealOpenMM devdw_from_der = -DOT3(force[probe_atom],dx);
    if(verbose_level > 0){
      cout << "Evdw Change from Direct: " << evdw_new - evdw << endl;
      cout << "Evdw Change from Deriv : " << devdw_from_der  << endl;
    }
#endif

#ifdef NOTNOW
    //
    // test derivative of the GB energy at constant self volumes
    //
    int probe_atom = 120;
    RealVec dx = RealVec(0.00, 0.00, 0.01);
    pos[probe_atom] += dx;
    //recompute Born radii w/o changing volume scaling factors
    for(int i = 0; i < numParticles; i++){
      inverse_born_radius[i] = 1./(radii[i] - AGBNP_RADIUS_INCREMENT);
      for(int j = 0; j < numParticles; j++){
	if(i == j) continue;
	if(ishydrogen[j]>0) continue;
	RealOpenMM b = (radii[i] - AGBNP_RADIUS_INCREMENT)/radii[j];
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d = sqrt(dist.dot(dist));
	if(d < AGBNP_I4LOOKUP_MAXA){
	  inverse_born_radius[i] -= pifac*volume_scaling_factor[j]*i4_lut->eval(d, b); 
	}	
      }
      RealOpenMM fp;
      born_radius[i] = 1./agbnp_swf_invbr(inverse_born_radius[i], fp);
      inverse_born_radius_fp[i] = fp;
    }
    //new GB energy
    RealOpenMM gb_self_energy_new = 0.0;
    RealOpenMM gb_pair_energy_new = 0.0;
    for(int i = 0; i < numParticles; i++){
      gb_self_energy_new += dielectric_factor*charge[i]*charge[i]/born_radius[i];
      for(int j = i+1; j < numParticles; j++){
	RealVec dist = pos[j] - pos[i];
	RealOpenMM d2 = dist.dot(dist);
	RealOpenMM qq = dielectric_factor*charge[j]*charge[i];
	RealOpenMM bb = born_radius[i]*born_radius[j];
	RealOpenMM etij = exp(-pt25*d2/bb);
	RealOpenMM fgb = 1./sqrt(d2 + bb*etij);
	RealOpenMM egb = 2.*qq*fgb;
	gb_pair_energy_new += egb;
      }
    }
    if(verbose_level > 0){
      cout << "GB self energy new: " << gb_self_energy_new << endl;
      cout << "GB pair energy new: " << gb_pair_energy_new << endl;
      cout << "GB energy new: " << gb_pair_energy_new+gb_self_energy_new << endl;
    }
    RealOpenMM degb_from_der = -DOT3(force[probe_atom],dx);
    if(verbose_level > 0){
      cout << "Egb Change from Direct: " << gb_pair_energy_new+gb_self_energy_new - (gb_pair_energy+gb_self_energy) << endl;
      cout << "Egb Change from Deriv : " << degb_from_der  << endl;
    }
#endif


    RealOpenMM volume_tmp, vol_energy_tmp;
    //set up the parameters of the pseudo-volume energy function and
    //compute the component of the gradient of Evdw due to the variations
    //of self volumes
    for(int i = 0; i < numParticles; i++){
      RealOpenMM vol = 4.*M_PI*pow(radii_vdw[i],3)/3.0; 
      nu[i] = evdw_der_W[i]/vol;
    }
    gvol->rescan_tree_gammas(nu);
    gvol->compute_volume(pos, volume_tmp, vol_energy_tmp, vol_force, free_volume, self_volume);
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_vdw;
    }

    //set up the parameters of the pseudo-volume energy function and
    //compute the component of the gradient of Egb due to the variations
    //of self volumes
    for(int i = 0; i < numParticles; i++){
      RealOpenMM vol = 4.*M_PI*pow(radii_vdw[i],3)/3.0; 
      nu[i] = egb_der_U[i]/vol;
    }
    gvol->rescan_tree_gammas(nu);
    gvol->compute_volume(pos, volume_tmp, vol_energy_tmp, vol_force, free_volume, self_volume);
    for(int i = 0; i < numParticles; i++){
      force[i] += vol_force[i] * w_egb;
    }


    if(verbose_level > 3){
      //creates input for test program
      double nm2ang = 10.0;
      double kjmol2kcalmol = 1/4.184;
      double gf = kjmol2kcalmol/(nm2ang*nm2ang);
      cout << "---- input for test program begins ----" << endl;
      cout << numParticles << endl;
      for(int i = 0; i < numParticles; i++){
	cout << std::setprecision(6) << std::setw(5) << i << " " << std::setw(12) << nm2ang*pos[i][0] << " " << std::setw(12) << nm2ang*pos[i][1] << " " << std::setw(12) << nm2ang*pos[i][2] << " " << std::setw(12) << nm2ang*radii_vdw[i] << " " << std::setw(12) << charge[i] << " " << std::setw(12) << gf*gammas[i] << " " << std::setw(2) << ishydrogen[i] << endl;
      }
      cout << "--- input for test program ends ----" << endl;
    }


    if(verbose_level > 3){
      //creates input for mkws program
      double nm2ang = 10.0;
      cout << "---- input for mkws program begins ----" << endl;
      cout << numParticles << endl;
      for(int i = 0; i < numParticles; i++){
	string at_symbol = "A";
	if(ishydrogen[i] > 0){
	  at_symbol = "H";
	}
	cout << std::setprecision(6) << std::setw(5) << i << " " << at_symbol << "" << std::setw(12) << nm2ang*pos[i][0] << " " << std::setw(12) << nm2ang*pos[i][1] << " " << std::setw(12) << nm2ang*pos[i][2] << " " << std::setw(12) << nm2ang*radii_vdw[i] << endl;
      }
      cout << "--- input for mkws program ends ----" << endl;
    }
    
    //returns energy
    return (double)energy;
}

double ReferenceCalcAGBNPForceKernel::executeAGBNP2(ContextImpl& context, bool includeForces, bool includeEnergy) {
  //weights
  RealOpenMM w_evol = 0.0, w_egb = 1.0, w_vdw = 1.0;
  
  vector<RealVec>& pos = extractPositions(context);
  vector<RealVec>& force = extractForces(context);
  RealOpenMM energy = 0.0;
  int verbose_level = 0;
  bool verbose = verbose_level > 0;
  
  if(verbose_level > 0) {
    cout << "Executing AGBNP2" << endl;
    cout << "-----------------------------------------------" << endl;
  } 


  //
  // self volumes with vdw radii
  //
  vector<RealOpenMM> nu(numParticles);
  for(int i = 0; i < numParticles; i++) nu[i] = 1.0;  //dummy gammas
  vector<RealOpenMM> volumes_vdw(numParticles);
  for(int i = 0; i < numParticles; i++){
    volumes_vdw[i] = ishydrogen[i]>0 ? 0.0 : 4.*M_PI*pow(radii_vdw[i],3)/3.;
  }
  gvol->compute_tree(pos, radii_vdw, volumes_vdw, nu);
  if(verbose_level > 5){
    gvol->print_tree();
  }
  RealOpenMM volume1, vol_energy1;
  gvol->compute_volume(pos, volume1, vol_energy1, vol_force, free_volume, self_volume);
  energy += w_evol * vol_energy1;
  
  if(verbose_level > 3){
      cout << "vol: " << volume1 << endl;
      cout << "energy_ms: " << vol_energy1 << endl;
  }
  
  //constructs molecular surface particles
  vector<MSParticle> msparticles1;
  double radw = solvent_radius;
  double volw = 4.*M_PI*pow(radw,3)/3.;
  double vol_coeff = 0.17;
  int nms = 0;
  for(int i = 0; i < numParticles; i++){
    if(ishydrogen[i]>0) continue;
    double rad1 = radii_vdw[i];
    for(int j = i + 1; j < numParticles; j++){
      if(ishydrogen[j]>0) continue;
      double rad2 = radii_vdw[j];
      double q = sqrt(rad1*rad2)/radw;
      RealVec dist = pos[j] - pos[i];
      RealOpenMM d = sqrt(dist.dot(dist));
      //      cout << d << " " << rad1 + rad2 << " " << rad1 + rad2 + 2*radw << endl;
      if(d < rad1 + rad2 + 2*radw && d > rad1 + rad2){
	//(d > rad1 + rad2) is a debug for now, need to find a way to turn off
	//pair ms volume when the two atoms overlap
	double dms = rad1 + rad2 + 0.5*radw;
	double volms0 = vol_coeff*q*q*volw;
	double sigma = 0.5*sqrt(q)*radw;
	double volms = volms0*exp(-0.5*(d-dms)*(d-dms)/(sigma*sigma));
	double distms_from_1 = 0.5*(d - rad1 - rad2) + rad1;//midpoint of the two surfaces
	RealVec distu = dist * (1/d);
	RealVec posms = distu * distms_from_1 + pos[i];
	if(verbose_level > 4){
	  cout << "S " << 10.0*posms[0] << " " << 10.0*posms[1] << " " << 10.0*posms[2] << " " << nms << " " << volms << endl;
	}
	MSParticle msp;
	msp.vol = volms;
	msp.pos = posms;
	msp.parent1 = i;
	msp.parent2 = j;
	msparticles1.push_back(msp);
	nms += 1;
      }
    }
  }

  //obtain free volumes of ms spheres by summing over atoms scaled by their self volumes
  //saves into new list those with non-zero volume
  vector<MSParticle> msparticles2;
  double ams = KFC/(solvent_radius*solvent_radius);
  GaussianVca gms, gatom, g12;
  for(int ims = 0; ims < msparticles1.size(); ims++){
    gms.a = ams;
    gms.v = msparticles1[ims].vol;
    gms.c = msparticles1[ims].pos;
    RealOpenMM freevolms = msparticles1[ims].vol;
    for(int i=0;i<numParticles;i++){
      RealOpenMM rad = radii_vdw[i];
      RealOpenMM ai = KFC/(rad*rad);
      RealOpenMM voli = self_volume[i];
      gatom.a = ai;
      gatom.v = voli;
      gatom.c = pos[i];
      RealOpenMM dVdr, dVdV, sfp;
      ogauss_alpha(gms, gatom, g12, dVdr, dVdV, sfp);
      freevolms -= g12.v;
    }
    if(freevolms > VOLMINMSA){
      MSParticle msp;
      RealOpenMM sp;
      msp.vol = freevolms*pol_switchfunc(freevolms, VOLMINMSA, VOLMINMSB, sp);
      msp.pos = gms.c;
      msp.parent1 = msparticles1[ims].parent1;
      msp.parent2 = msparticles1[ims].parent2;
      msparticles2.push_back(msp);
      if(verbose_level > 4){
	cout << "O " << 10.0*msp.pos[0] << " " << 10.0*msp.pos[1] << " " <<  10.0*msp.pos[2] << " " << ims << " " << msp.vol << endl;
      }
    }
  }

  if(verbose_level > 3){
    cout << "Number of ms particles: " << msparticles1.size() << endl;
    cout << "Number of ms particles with Vf > 0: " << msparticles2.size() << endl;
  }

  vector<RealOpenMM> svadd(numParticles);
  for(int iat=0;iat<numParticles;iat++){
    svadd[iat] = 0.0;
  }

  if(msparticles2.size() > 0){
  
    // now get the self-volumes of MS particles among themselves
    int num_ms = msparticles2.size();
    vector<RealOpenMM> radii_ms(num_ms);
    for(int i=0;i<num_ms;i++) radii_ms[i] = solvent_radius;
    vector<RealOpenMM> volumes_ms(num_ms);
    for(int i=0;i<num_ms;i++) volumes_ms[i] = msparticles2[i].vol;
    vector<RealOpenMM> gammas_ms(num_ms);
    for(int i=0;i<num_ms;i++) gammas_ms[i] = 1.;
    vector<int> ishydrogen_ms(num_ms);
    for(int i=0;i<num_ms;i++) ishydrogen_ms[i] = 0;
    vector<RealVec> pos_ms(num_ms);
    for(int i=0;i<num_ms;i++) pos_ms[i] = msparticles2[i].pos;
    GaussVol *gvolms = new GaussVol(msparticles2.size(), ishydrogen_ms);
    gvolms->compute_tree(pos_ms, radii_ms, volumes_ms, gammas_ms);
    vector<RealVec> forces_ms(num_ms);
    vector<RealOpenMM> freevols_ms(num_ms), selfvols_ms(num_ms);
    RealOpenMM vol_ms, energy_ms;
    gvolms->compute_volume(pos_ms, vol_ms, energy_ms, forces_ms, freevols_ms, selfvols_ms);

    for(int i=0;i<num_ms;i++){
      msparticles2[i].selfvol = selfvols_ms[i];
    }
    
    if(verbose_level > 3){
      cout << "vol_ms: " << vol_ms << endl;
      cout << "energy_ms: " << energy_ms << endl;

      cout << "MS Self Volumes:" << endl;
      for(int i=0;i<num_ms;i++){
	cout << i << " " << selfvols_ms[i] << endl;
      }
    }

    //add self-volumes to parents
    for(int i=0;i<num_ms;i++) {
      int iat = msparticles2[i].parent1;
      int jat = msparticles2[i].parent2;
      svadd[iat] += 0.5*msparticles2[i].selfvol;
      svadd[jat] += 0.5*msparticles2[i].selfvol;
    }
    
    if(verbose_level > 2){
      cout << "Updated Self Volumes:" << endl;
      for(int iat=0;iat<numParticles;iat++){
	double r = 0.;
	if(self_volume[iat] > 0){
	  r = 100.0*svadd[iat]/self_volume[iat];
	}
	RealOpenMM rad = radii_vdw[iat];
	RealOpenMM vol = (4./3.)*M_PI*rad*rad*rad;
	cout << "SV " <<  iat << " " << self_volume[iat] << " " << self_volume[iat]+svadd[iat] << " " << r << " " << self_volume[iat]/vol << endl;
      }
    }

    for(int iat=0;iat<numParticles;iat++){
      self_volume[iat] += svadd[iat];
    }
    
  }



  //volume scaling factors from self volumes (with small radii)
  RealOpenMM tot_vol = 0;
  for(int i = 0; i < numParticles; i++){
    RealOpenMM rad = radii_vdw[i];
    RealOpenMM vol = (4./3.)*M_PI*rad*rad*rad;
    volume_scaling_factor[i] = self_volume[i]/vol;
    tot_vol += self_volume[i];
  }
  if(verbose_level > 0){
    cout << "Volume from self volumes + MS self volumes: " << tot_vol << endl;
  }

  //compute inverse Born radii, prototype, no cutoff
  RealOpenMM pifac = 1./(4.*M_PI);
  for(int i = 0; i < numParticles; i++){
    RealOpenMM rad = radii_vdw[i];
    RealOpenMM rad_add = pifac*svadd[i]/(rad*rad);//from Taylor expansion
    if(verbose_level > 3){
      cout << "Radd " << i << " " << 10*rad_add << endl;
    }
    inverse_born_radius[i] = 1./(rad + rad_add);
    for(int j = 0; j < numParticles; j++){
      if(i == j) continue;
      if(ishydrogen[j] > 0) continue;
      RealVec dist = pos[j] - pos[i];
      RealOpenMM d = sqrt(dist.dot(dist));
      if(d < AGBNP_I4LOOKUP_MAXA){
	int rad_typei = i4_lut->radius_type_screened[i];
	int rad_typej = i4_lut->radius_type_screener[j];
	inverse_born_radius[i] -= pifac*volume_scaling_factor[j]*i4_lut->eval(d, rad_typei, rad_typej);
      }	
    }
    RealOpenMM fp;
    born_radius[i] = 1./agbnp_swf_invbr(inverse_born_radius[i], fp);
    inverse_born_radius_fp[i] = fp;
  }

  if(verbose_level > 2){
    cout << "Born radii:" << endl;
    RealOpenMM fp;
    for(int i = 0; i < numParticles; i++){
      cout << "BR " << i << " " << 10.*born_radius[i] << " Si " << volume_scaling_factor[i] << endl;
    }
  }

  //GB energy
  RealOpenMM dielectric_in = 1.0;
  RealOpenMM dielectric_out= 80.0;
  RealOpenMM tokjmol = 4.184*332.0/10.0; //the factor of 10 is the conversion of 1/r from nm to Ang
  RealOpenMM dielectric_factor = tokjmol*(-0.5)*(1./dielectric_in - 1./dielectric_out);
  RealOpenMM pt25 = 0.25;
  vector<RealOpenMM> egb_der_Y(numParticles);
  for(int i = 0; i < numParticles; i++){
    egb_der_Y[i] = 0.0;
  }
  RealOpenMM gb_self_energy = 0.0;
  RealOpenMM gb_pair_energy = 0.0;
  for(int i = 0; i < numParticles; i++){
    double uself = dielectric_factor*charge[i]*charge[i]/born_radius[i];
    gb_self_energy += uself;
    for(int j = i+1; j < numParticles; j++){
      RealVec dist = pos[j] - pos[i];
      RealOpenMM d2 = dist.dot(dist);
      RealOpenMM qqf = charge[j]*charge[i];
      RealOpenMM qq = dielectric_factor*qqf;
      RealOpenMM bb = born_radius[i]*born_radius[j];
      RealOpenMM etij = exp(-pt25*d2/bb);
      RealOpenMM fgb = 1./sqrt(d2 + bb*etij);
      RealOpenMM egb = 2.*qq*fgb;
      gb_pair_energy += egb;
      RealOpenMM fgb3 = fgb*fgb*fgb;
      RealOpenMM mw = -2.0*qq*(1.0-pt25*etij)*fgb3;
      RealVec g = dist * mw;
      force[i] += g * w_egb;
      force[j] -= g * w_egb;
      RealOpenMM ytij = qqf*(bb+pt25*d2)*etij*fgb3;
      egb_der_Y[i] += ytij;
      egb_der_Y[j] += ytij;
    }
  }
  if(verbose_level > 0){
    cout << "GB self energy: " << gb_self_energy << endl;
    cout << "GB pair energy: " << gb_pair_energy << endl;
    cout << "GB energy: " << gb_pair_energy+gb_self_energy << endl;
  }
  energy += w_egb*gb_pair_energy + w_egb*gb_self_energy;
  
  if(verbose_level > 3){
    cout << "Y parameters: " << endl;
    for(int i = 0; i < numParticles; i++){
      cout << "Y: " << i << " " << egb_der_Y[i] << endl;
    }
  }

  //compute van der Waals energy
  RealOpenMM evdw = 0.;
  for(int i=0;i<numParticles; i++){
    evdw += vdw_alpha[i]/pow(born_radius[i]+AGBNP_HB_RADIUS,3);
  }
  if(verbose_level > 0){
    cout << "Van der Waals energy: " << evdw << endl;
  }
  energy += w_vdw*evdw;
  
  //compute atom-level property for the calculation of the gradients of Evdw and Egb
  vector<RealOpenMM> evdw_der_brw(numParticles);
  for(int i = 0; i < numParticles; i++){
    RealOpenMM br = born_radius[i];
    evdw_der_brw[i] = -pifac*3.*vdw_alpha[i]*br*br*inverse_born_radius_fp[i]/pow(br+AGBNP_HB_RADIUS,4);
  }
  if(verbose_level > 3){
    cout << "BrW parameters: " << endl;
    for(int i = 0; i < numParticles; i++){
      cout << "BrW: " << i << " " << evdw_der_brw[i] << endl;      
    }
  }
  
    
  vector<RealOpenMM> egb_der_bru(numParticles);
  for(int i = 0; i < numParticles; i++){
    RealOpenMM br = born_radius[i];
    RealOpenMM qi = charge[i];
    egb_der_bru[i] = -pifac*dielectric_factor*(qi*qi + egb_der_Y[i]*br)*inverse_born_radius_fp[i];
  }
  
  if(verbose_level > 3){
    cout << "BrU parameters: " << endl;
    for(int i = 0; i < numParticles; i++){
      cout << "BrU: " << i << " " << egb_der_bru[i] << endl;      
    }
  }
  
  
  //compute the component of the gradients of the van der Waals and GB energies due to
  //variations of Born radii
  //also accumulates W's and U's for self-volume components of the gradients later
  vector<RealOpenMM> evdw_der_W(numParticles);
  vector<RealOpenMM> egb_der_U(numParticles);
  for(int i = 0; i < numParticles; i++){
    evdw_der_W[i] = egb_der_U[i] = 0.0;
  }
  for(int i = 0; i < numParticles; i++){
    for(int j = 0; j < numParticles; j++){
      if(i == j) continue;
      if(ishydrogen[j]>0) continue;
      RealVec dist = pos[j] - pos[i];
      RealOpenMM d = sqrt(dist.dot(dist));
      double Qji = 0.0, dQji = 0.0;
      // Qji: j descreens i
      if(d < AGBNP_I4LOOKUP_MAXA){
	int rad_typei = i4_lut->radius_type_screened[i];
	int rad_typej = i4_lut->radius_type_screener[j];
	Qji = i4_lut->eval(d, rad_typei, rad_typej); 
	dQji = i4_lut->evalderiv(d, rad_typei, rad_typej); 
      }
      RealVec w;
      //van der Waals stuff
      evdw_der_W[j] += evdw_der_brw[i]*Qji;
      w = dist * evdw_der_brw[i]*volume_scaling_factor[j]*dQji/d;
      force[i] += w * w_vdw;
      force[j] -= w * w_vdw;
      //GB stuff
      egb_der_U[j] += egb_der_bru[i]*Qji;
      w = dist * egb_der_bru[i]*volume_scaling_factor[j]*dQji/d;
      force[i] += w * w_egb;
      force[j] -= w * w_egb;
    }
  }
  
  //returns energy
  if(verbose) cout << "energy: " << energy << endl;
  return (double)energy;
}


void ReferenceCalcAGBNPForceKernel::copyParametersToContext(ContextImpl& context, const AGBNPForce& force) {
  if (force.getNumParticles() != numParticles)
    throw OpenMMException("updateParametersInContext: The number of AGBNP particles has changed");

  for (int i = 0; i < numParticles; i++){
    double r, g, alpha, q;
    bool h;
    force.getParticleParameters(i, r, g, alpha, q, h);
    if(pow(radii_vdw[i]-r,2) > 1.e-6){
      throw OpenMMException("updateParametersInContext: AGBNP plugin does not support changing atomic radii.");
    }
    if(h && ishydrogen[i] == 0){
      throw OpenMMException("updateParametersInContext: AGBNP plugin does not support changing heavy/hydrogen atoms.");
    }
    gammas[i] = g;
    if(h) gammas[i] = 0.0;
    vdw_alpha[i] = alpha;
    charge[i] = q;
  }
}
