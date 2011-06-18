/**
 * \file sensorAbsloc.hpp
 *
 * Header file for absolute localisation sensors (gps, motion capture...)
 *
 * \date 16/03/2011
 * \author croussil
 *
 * \ingroup rtslam
 */

#ifndef SENSORABSLOC_HPP_
#define SENSORABSLOC_HPP_

#include "jmath/jblas.hpp"
#include "jmath/ublasExtra.hpp"
#include "jmath/misc.hpp"

#include "rtslam/rtSlam.hpp"
#include "rtslam/quatTools.hpp"
#include "rtslam/sensorAbstract.hpp"
#include "rtslam/innovation.hpp"

namespace jafar {
	namespace rtslam {

		class SensorAbsloc;
		typedef boost::shared_ptr<SensorAbsloc> absloc_ptr_t;
		
		/**
		 * Class for absolute localization sensors (gps, motion capture...)
		 * For now we assume that we have at least one reading before images and
		 * that is is very precise. Improvements would be to start at 0,0,0
		 * with uncertainty 0 and estimate the initial position.
		 * \ingroup rtslam
		 */
		class SensorAbsloc: public SensorProprioAbstract {
			protected:
				jblas::ind_array ia_rs;
				Innovation *innovation;
				Measurement *measurement;
				Expectation *expectation;
				jblas::mat EXP_rs;
				jblas::mat INN_rs;
				jblas::mat EXP_q;
				bool hasVar;
				int inns;
				bool absolute;
			public:
				SensorAbsloc(const robot_ptr_t & robPtr, const filtered_obj_t inFilter = UNFILTERED, bool absolute = false):
				  SensorProprioAbstract(robPtr, inFilter),
					ia_rs(ia_globalPose), innovation(NULL), measurement(NULL), expectation(NULL),
				  hasVar(false), inns(0), absolute(absolute)
				{}
				~SensorAbsloc() { delete innovation; delete measurement; }
				virtual void setHardwareSensor(hardware::hardware_sensorprop_ptr_t hardwareSensorPtr_)
				{
					hardwareSensorPtr = hardwareSensorPtr_;
					// initialize jacobians and innovation sizes
					inns = hardwareSensorPtr->dataSize();
					innovation = new Innovation(inns);
					measurement = new Measurement(inns);
					expectation = new Expectation(inns);
					EXP_rs.resize(inns, ia_rs.size(), false);
					INN_rs.resize(inns, ia_rs.size(), false);
					EXP_q.resize(inns, 4, false);
					if (hardwareSensorPtr->varianceSize() == hardwareSensorPtr->dataSize())
						hasVar = true;
				}
				
				
				virtual void init(unsigned id)
				{
					RawInfos infos;
					queryAvailableRaws(infos);
					
					// find minimum variance
					jblas::vec3 min_var; min_var(0) = min_var(1) = min_var(2) = 1e3;
					for(std::vector<RawInfo>::iterator it = infos.available.begin(); it != infos.available.end(); ++it)
					{
						hardwareSensorPtr->observeRaw((*it).id, reading);
						for(int i = 0; i < 3; ++i) if (reading.data(i+1+inns) < min_var(i)) min_var(i) = reading.data(i+1+inns);
						if ((*it).id == id) break;
					}
					
					// do the average using only readings with variance between min and 2*min
					jblas::vec3 average; average.clear();
					jblas::vec3 sum_coeffs; sum_coeffs.clear();
					for(std::vector<RawInfo>::iterator it = infos.available.begin(); it != infos.available.end(); ++it)
					{
						hardwareSensorPtr->observeRaw((*it).id, reading);
						for(int i = 0; i < 3; ++i)
							if (reading.data(i+1+inns) < 2*min_var(i))
								{ average(i) += reading.data(i+1)*reading.data(i+1+inns); sum_coeffs(i) += reading.data(i+1+inns); }
						if ((*it).id == id) break;
					}
					for(int i = 0; i < 3; ++i) average(i) /= sum_coeffs(i);
					
					// now initialize the robot state with this variance
					for(int i = 0; i < 3; ++i) { reading.data(i+1) = average(i); reading.data(i+1+inns) = min_var(i); }
				}
				

				virtual void process(unsigned id)
				{
					if (use_for_init)
					{
						init(id);
					} else
					{
						hardwareSensorPtr->getRaw(id, reading);
					}
						
					// TODO a vec hardware sensor should return some information about what it is returning
					// just like robots should be able to do with, including whether the info is measure or variance.
					// pos(x/y/z)(gps,baro,mocap), ori (mocap,mag), vel(gps), gyr(y/p/r), acc(x/y/z)

					bool first = false;
					if (robotPtr()->origin.size() == 0)
					{
						first = true;
						robotPtr()->origin.resize(3, false);
						robotPtr()->origin.clear();
					}
					
					jblas::vec T = ublas::subrange(pose.x(), 0, 3);
					jblas::vec p = ublas::subrange(robotPtr()->pose.x(), 0, 3);
					jblas::vec q = ublas::subrange(robotPtr()->pose.x(), 3, 7);
					jblas::vec Tr = quaternion::rotate(q,T);

					switch (innovation->size())
					{
						case 3: // POS only
							quaternion::rotate_by_dq(q, T, EXP_q);
							ublas::subrange(EXP_rs, 0,3, 0,3) = jblas::identity_mat(3);
							ublas::subrange(EXP_rs, 0,3, 3,7) = EXP_q;
						
							expectation->x() = p + Tr;
							expectation->P() = ublasExtra::prod_JPJt(ublas::project(robotPtr()->mapPtr()->filterPtr->P(), ia_rs, ia_rs), EXP_rs);
							
							measurement->x()(0) = reading.data(2) - robotPtr()->origin(0);
							measurement->x()(1) = reading.data(1) - robotPtr()->origin(1);
							measurement->x()(2) = reading.data(3) - robotPtr()->origin(2);
							if (hasVar)
							{
								measurement->P()(0,0) = jmath::sqr(reading.data(2+inns));
								measurement->P()(1,1) = jmath::sqr(reading.data(1+inns));
								measurement->P()(2,2) = jmath::sqr(reading.data(3+inns));
							} else
							{
								// TODO
								JFR_ERROR(RtslamException, RtslamException::GENERIC_ERROR,
								          "SensorAbsloc with constant uncertainty not implemented yet");
							}
							
							// TODO gating ?
							
							innovation->x() = measurement->x() - expectation->x();
							innovation->P() = measurement->P() + expectation->P();
							INN_rs = -EXP_rs;
							break;
						default: // TODO 7=pos+ori
							JFR_ERROR(RtslamException, RtslamException::GENERIC_ERROR,
							          "SensorAbsloc reading size " << reading.data.size() << " not supported.");
					}

					if (first)
					{
						// for first reading we force initialization
						if (absolute)
						{
							switch (innovation->size())
							{
								case 3: // POS
									robotPtr()->origin = jblas::zero_vec(3);
									ublas::subrange(robotPtr()->pose.x(), 0,3) = measurement->x() - Tr;
									ublas::subrange(robotPtr()->pose.P(), 0,3, 0,3) = measurement->P() + 
										ublasExtra::prod_JPJt(ublas::subrange(robotPtr()->pose.P(), 3,7, 3,7), EXP_q);
									break;
								default:
									break;
							}
						} else
						{
							switch (innovation->size())
							{
								case 3: // POS
									robotPtr()->origin = measurement->x() - Tr;
									ublas::subrange(robotPtr()->pose.x(), 0, 3) = jblas::zero_vec(3);
									ublas::subrange(robotPtr()->pose.P(), 0,3, 0,3) = measurement->P() + 
										ublasExtra::prod_JPJt(ublas::subrange(robotPtr()->pose.P(), 3,7, 3,7), EXP_q);
									break;
								default:
									break;
							}
						}
						
						std::cout << std::setprecision(16) << "robot origin: " << robotPtr()->origin << 
							" ; initial position: " << ublas::subrange(robotPtr()->pose.x(), 0,3) << 
							" ; initial pose var: " << ublas::subrange(robotPtr()->pose.P(), 0,3, 0,3) << std::endl;
						
					} else
					{
						map_ptr_t mapPtr = robotPtr()->mapPtr();
						ind_array ia_x = mapPtr->ia_used_states();
						mapPtr->filterPtr->correct(ia_x,*innovation,INN_rs,ia_rs);
					}

					if (use_for_init)
					{
						use_for_init = false;
						hardwareSensorPtr->getRaw(id, reading); // just to free
					}
					
					//hardwareSensorPtr->release();
				}

				
		};
}}

#endif
