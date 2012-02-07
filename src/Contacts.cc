/*
 * RBDL - Rigid Body Library
 * Copyright (c) 2011 Martin Felis <martin.felis@iwr.uni-heidelberg.de>
 *
 * Licensed under the zlib license. See LICENSE for more details.
 */

#include <iostream>
#include <limits>
#include <assert.h>

#include "mathutils.h"
#include "Logging.h"

#include "Model.h"
#include "Joint.h"
#include "Body.h"
#include "Contacts.h"
#include "Dynamics.h"
#include "Dynamics_experimental.h"
#include "Kinematics.h"

using namespace SpatialAlgebra;
using namespace SpatialAlgebra::Operators;

namespace RigidBodyDynamics {

unsigned int ConstraintSet::AddConstraint (
		unsigned int body_id,
		const Vector3d &body_point,
		const Vector3d &world_normal,
		const char *constraint_name,
		double acceleration) {
	assert (bound == false);

	std::string name_str;
	if (constraint_name != NULL)
		name_str = constraint_name;

	name.push_back (name_str);
	body.push_back (body_id);
	point.push_back (body_point);
	normal.push_back (world_normal);

	unsigned int n_constr = constraint_acceleration.size() + 1;

	constraint_acceleration.conservativeResize (n_constr);
	constraint_acceleration[n_constr - 1] = acceleration;

	constraint_force.conservativeResize (n_constr);
	constraint_force[n_constr - 1] = 0.;

	return n_constr - 1;
}

bool ConstraintSet::Bind (const Model &model) {
	assert (bound == false);

	unsigned int n_constr = size();

	H.conservativeResize (model.dof_count, model.dof_count);
	C.conservativeResize (model.dof_count);
	gamma.conservativeResize (n_constr);
	G.conservativeResize (n_constr, model.dof_count);
	A.conservativeResize (model.dof_count + n_constr, model.dof_count + n_constr);
	b.conservativeResize (model.dof_count + n_constr);
	x.conservativeResize (model.dof_count + n_constr);

	K.conservativeResize (n_constr, n_constr);
	a.conservativeResize (n_constr);
	QDDot_t.conservativeResize (model.dof_count);
	QDDot_0.conservativeResize (model.dof_count);
	f_t.resize (n_constr, SpatialVectorZero);
	f_ext_constraints.resize (model.mBodies.size(), SpatialVectorZero);
	point_accel_0.resize (n_constr, Vector3d::Zero());

	d_pA = std::vector<SpatialVector> (model.mBodies.size(), SpatialVectorZero);
	d_a = std::vector<SpatialVector> (model.mBodies.size(), SpatialVectorZero);
	d_u = VectorNd::Zero (model.mBodies.size());

	d_IA = std::vector<SpatialMatrix> (model.mBodies.size(), SpatialMatrixIdentity);
	d_U = std::vector<SpatialVector> (model.mBodies.size(), SpatialVectorZero);
	d_d = VectorNd::Zero (model.mBodies.size());

	bound = true;

	return bound;
}

void ConstraintSet::clear() {
	constraint_acceleration.setZero();
	constraint_force.setZero();

	H.setZero();
	C.setZero();
	gamma.setZero();
	G.setZero();
	A.setZero();
	b.setZero();
	x.setZero();

	K.setZero();
	a.setZero();
	QDDot_t.setZero();
	QDDot_0.setZero();

	unsigned int i;
	for (i = 0; i < f_t.size(); i++)
		f_t[i].setZero();

	for (i = 0; i < f_ext_constraints.size(); i++)
		f_ext_constraints[i].setZero();

	for (i = 0; i < point_accel_0.size(); i++)
		point_accel_0[i].setZero();

	for (i = 0; i < d_pA.size(); i++)
		d_pA[i].setZero();

	for (i = 0; i < d_a.size(); i++)
		d_a[i].setZero();

	d_u.setZero();
}

void ForwardDynamicsContactsLagrangian (
		Model &model,
		const VectorNd &Q,
		const VectorNd &QDot,
		const VectorNd &Tau,
		ConstraintSet &CS,
		VectorNd &QDDot
		) {
	LOG << "-------- " << __func__ << " --------" << std::endl;
	// Compute C
	CS.QDDot_0.setZero();
	InverseDynamics (model, Q, QDot, CS.QDDot_0, CS.C);

	assert (CS.H.cols() == model.dof_count && CS.H.rows() == model.dof_count);

	// Compute H
	CompositeRigidBodyAlgorithm (model, Q, CS.H, false);
	
	// Compute G
	unsigned int i,j;

	// variables to check whether we need to recompute G
	unsigned int prev_body_id = 0;
	Vector3d prev_body_point = Vector3d::Zero();
	MatrixNd Gi (3, model.dof_count);

	for (i = 0; i < CS.size(); i++) {
		// Only alow contact normals along the coordinate axes
		unsigned int axis_index = 0;

		if (CS.normal[i] == Vector3d(1., 0., 0.))
			axis_index = 0;
		else if (CS.normal[i] == Vector3d(0., 1., 0.))
			axis_index = 1;
		else if (CS.normal[i] == Vector3d(0., 0., 1.))
			axis_index = 2;
		else
			assert (0 && "Invalid contact normal axis!");

		// only compute the matrix Gi if actually needed
		if (prev_body_id != CS.body[i] || prev_body_point != CS.point[i]) {
			CalcPointJacobian (model, Q, CS.body[i], CS.point[i], Gi, false);
			prev_body_id = CS.body[i];
			prev_body_point = CS.point[i];
		}

		for (j = 0; j < model.dof_count; j++) {
			CS.G(i,j) = Gi(axis_index, j);
		}
	}

	// Compute gamma
	prev_body_id = 0;
	prev_body_point = Vector3d::Zero();
	Vector3d gamma_i = Vector3d::Zero();

	// update Kinematics just once
	UpdateKinematics (model, Q, QDot, CS.QDDot_0);

	for (i = 0; i < CS.size(); i++) {
		// Only alow contact normals along the coordinate axes
		unsigned int axis_index = 0;

		if (CS.normal[i] == Vector3d(1., 0., 0.))
			axis_index = 0;
		else if (CS.normal[i] == Vector3d(0., 1., 0.))
			axis_index = 1;
		else if (CS.normal[i] == Vector3d(0., 0., 1.))
			axis_index = 2;
		else
			assert (0 && "Invalid contact normal axis!");

		// only compute point accelerations when necessary
		if (prev_body_id != CS.body[i] || prev_body_point != CS.point[i]) {
			gamma_i = CalcPointAcceleration (model, Q, QDot, CS.QDDot_0, CS.body[i], CS.point[i], false);
			prev_body_id = CS.body[i];
			prev_body_point = CS.point[i];
		}
	
		// we also substract ContactData[i].acceleration such that the contact
		// point will have the desired acceleration
		CS.gamma[i] = gamma_i[axis_index] - CS.constraint_acceleration[i];
	}
	
	// Build the system
	CS.A.setZero();
	CS.b.setZero();
	CS.x.setZero();

	// Build the system: Copy H
	for (i = 0; i < model.dof_count; i++) {
		for (j = 0; j < model.dof_count; j++) {
			CS.A(i,j) = CS.H(i,j);	
		}
	}

	// Build the system: Copy G, and G^T
	for (i = 0; i < CS.size(); i++) {
		for (j = 0; j < model.dof_count; j++) {
			CS.A(i + model.dof_count, j) = CS.G (i,j);
			CS.A(j, i + model.dof_count) = CS.G (i,j);
		}
	}

	// Build the system: Copy -C + \tau
	for (i = 0; i < model.dof_count; i++) {
		CS.b[i] = -CS.C[i] + Tau[i];
	}

	// Build the system: Copy -gamma
	for (i = 0; i < CS.size(); i++) {
		CS.b[i + model.dof_count] = - CS.gamma[i];
	}

	LOG << "A = " << std::endl << CS.A << std::endl;
	LOG << "b = " << std::endl << CS.b << std::endl;

#ifndef RBDL_USE_SIMPLE_MATH
	switch (CS.linear_solver) {
		case (ConstraintSet::LinearSolverPartialPivLU) :
			CS.x = CS.A.partialPivLu().solve(CS.b);
			break;
		case (ConstraintSet::LinearSolverColPivHouseholderQR) :
			CS.x = CS.A.colPivHouseholderQr().solve(CS.b);
			break;
		default:
			LOG << "Error: Invalid linear solver: " << CS.linear_solver << std::endl;
			assert (0);
			break;
	}
#else
	bool solve_successful = LinSolveGaussElimPivot (CS.A, CS.b, CS.x);
	assert (solve_successful);
#endif

	LOG << "x = " << std::endl << CS.x << std::endl;

	// Copy back QDDot
	for (i = 0; i < model.dof_count; i++)
		QDDot[i] = CS.x[i];

	// Copy back contact forces
	for (i = 0; i < CS.size(); i++) {
		CS.constraint_force[i] = CS.x[model.dof_count + i];
	}
}

void ComputeContactImpulsesLagrangian (
		Model &model,
		const VectorNd &Q,
		const VectorNd &QDotMinus,
		std::vector<ContactInfo> &ContactData,
		VectorNd &QDotPlus
		) {
	LOG << "-------- " << __func__ << " --------" << std::endl;

	// Compute H
	MatrixNd H (model.dof_count, model.dof_count);

	VectorNd QZero = VectorNd::Zero (model.dof_count);
	UpdateKinematics (model, Q, QZero, QZero);
	CompositeRigidBodyAlgorithm (model, Q, H, false);

	// Compute G
	MatrixNd G (ContactData.size(), model.dof_count);

	unsigned int i,j;

	// variables to check whether we need to recompute G
	unsigned int prev_body_id = 0;
	Vector3d prev_body_point = Vector3d::Zero();
	MatrixNd Gi (3, model.dof_count);

	for (i = 0; i < ContactData.size(); i++) {
		// Only alow contact normals along the coordinate axes
		unsigned int axis_index = 0;

		if (ContactData[i].normal == Vector3d(1., 0., 0.))
			axis_index = 0;
		else if (ContactData[i].normal == Vector3d(0., 1., 0.))
			axis_index = 1;
		else if (ContactData[i].normal == Vector3d(0., 0., 1.))
			axis_index = 2;
		else
			assert (0 && "Invalid contact normal axis!");

		// only compute the matrix Gi if actually needed
		if (prev_body_id != ContactData[i].body_id || prev_body_point != ContactData[i].point) {
			CalcPointJacobian (model, Q, ContactData[i].body_id, ContactData[i].point, Gi, false);
			prev_body_id = ContactData[i].body_id;
			prev_body_point = ContactData[i].point;
		}

		for (j = 0; j < model.dof_count; j++) {
			G(i,j) = Gi(axis_index, j);
		}
	}

	// Compute H * \dot{q}^-
	VectorNd Hqdotminus (H * QDotMinus);

	// Build the system
	MatrixNd A = MatrixNd::Constant (model.dof_count + ContactData.size(), model.dof_count + ContactData.size(), 0.);
	VectorNd b = VectorNd::Constant (model.dof_count + ContactData.size(), 0.);
	VectorNd x = VectorNd::Constant (model.dof_count + ContactData.size(), 0.);

	// Build the system: Copy H
	for (i = 0; i < model.dof_count; i++) {
		for (j = 0; j < model.dof_count; j++) {
			A(i,j) = H(i,j);	
		}
	}

	// Build the system: Copy G, and G^T
	for (i = 0; i < ContactData.size(); i++) {
		for (j = 0; j < model.dof_count; j++) {
			A(i + model.dof_count, j) = G (i,j);
			A(j, i + model.dof_count) = G (i,j);
		}
	}

	// Build the system: Copy -C + \tau
	for (i = 0; i < model.dof_count; i++) {
		b[i] = Hqdotminus[i];
	}

	// Build the system: Copy -gamma
	for (i = 0; i < ContactData.size(); i++) {
		b[i + model.dof_count] = ContactData[i].acceleration;
	}
	
	// Solve the system
#ifndef RBDL_USE_SIMPLE_MATH
	x = A.colPivHouseholderQr().solve (b);
#else
	bool solve_successful = LinSolveGaussElimPivot (A, b, x);
	assert (solve_successful);
#endif

	// Copy back QDDot
	for (i = 0; i < model.dof_count; i++)
		QDotPlus[i] = x[i];

	// Copy back contact impulses
	for (i = 0; i < ContactData.size(); i++) {
		ContactData[i].force = x[model.dof_count + i];
	}

}

/** \brief Compute only the effects of external forces on the generalized accelerations
 *
 * This function is a reduced version of ForwardDynamics() which only
 * computes the effects of the external forces on the generalized
 * accelerations.
 *
 */
void ForwardDynamicsApplyConstraintForces (
		Model &model,
		ConstraintSet &CS,
		VectorNd &QDDot
		) {
	LOG << "-------- " << __func__ << " --------" << std::endl;

	assert (QDDot.size() == model.dof_count);

	unsigned int i;

	for (i = 1; i < model.mBodies.size(); i++) {
		CS.d_pA[i] = crossf(model.v[i], model.mBodies[i].mSpatialInertia * model.v[i]);
		CS.d_IA[i] = model.mBodies[i].mSpatialInertia;

		if (CS.f_ext_constraints[i] != SpatialVectorZero) {
			CS.d_pA[i] -= spatial_adjoint(model.X_base[i].toMatrix()) * (CS.f_ext_constraints)[i];
//			LOG << "f_t (local)[" << i << "] = " << spatial_adjoint(model.X_base[i]) * (*f_ext)[i] << std::endl;
		}
//		LOG << "i = " << i << " d_p[i] = " << d_p[i].transpose() << std::endl;
	}

	for (i = model.mBodies.size() - 1; i > 0; i--) {
		// we can skip further processing if the joint is fixed
		if (model.mJoints[i].mJointType == JointTypeFixed)
			continue;

		CS.d_U[i] = CS.d_IA[i] * model.S[i];
		CS.d_d[i] = model.S[i].dot(model.U[i]);
		CS.d_u[i] = model.tau[i] - model.S[i].dot(CS.d_pA[i]);

		unsigned int lambda = model.lambda[i];
		if (lambda != 0) {
			SpatialMatrix Ia = CS.d_IA[i] - CS.d_U[i] * (CS.d_U[i] / CS.d_d[i]).transpose();
			SpatialVector pa = CS.d_pA[i] + Ia * model.c[i] + CS.d_U[i] * CS.d_u[i] / CS.d_d[i];
			SpatialTransform X_lambda = model.X_lambda[i];

			// note: X_lambda.inverse().spatial_adjoint() = X_lambda.transpose()
			CS.d_IA[lambda] = CS.d_IA[lambda] + X_lambda.toMatrixTranspose() * Ia * X_lambda.toMatrix();
			CS.d_pA[lambda] = CS.d_pA[lambda] + model.X_lambda[i].toMatrixTranspose() * pa;
		}
	}

	/*
	for (i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_IA[i] " << std::endl << d_IA[i] << std::endl;
	}
	*/

	for (unsigned int i = 0; i < CS.f_ext_constraints.size(); i++) {
		LOG << "f_ext[" << i << "] = " << (CS.f_ext_constraints)[i].transpose();
	}

	for (i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_pA[i] - pA[i] " << (CS.d_pA[i] - model.pA[i]).transpose();
	}
	for (i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_u[i] - u[i] = " << (CS.d_u[i] - model.u[i]) << std::endl;
	}
	for (i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_d[i] - d[i] = " << (CS.d_d[i] - model.d[i]) << std::endl;
	}
	for (i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_U[i] - U[i] = " << (CS.d_U[i] - model.U[i]).transpose() << std::endl;
	}

	SpatialVector spatial_gravity (0., 0., 0., model.gravity[0], model.gravity[1], model.gravity[2]);

	for (i = 1; i < model.mBodies.size(); i++) {
		unsigned int lambda = model.lambda[i];
		SpatialTransform X_lambda = model.X_lambda[i];

		if (lambda == 0) {
			CS.d_a[i] = X_lambda.apply(spatial_gravity * (-1.)) + model.c[i];
		} else {
			CS.d_a[i] = X_lambda.apply(CS.d_a[lambda]) + model.c[i];
		}

		// we can skip further processing if the joint type is fixed
		if (model.mJoints[i].mJointType == JointTypeFixed) {
			model.qddot[i] = 0.;
			continue;
		}

		QDDot[i - 1] = (CS.d_u[i] - model.U[i].dot(CS.d_a[i])) / model.d[i];
		LOG << "QDDot_t[" << i - 1 << "] = " << QDDot[i - 1] << std::endl;
		CS.d_a[i] = CS.d_a[i] + model.S[i] * QDDot[i - 1];
		LOG << "d_a[i] - a[i] = " << (CS.d_a[i] - X_lambda.apply(model.a[i])).transpose() << std::endl;
	}
}

void ForwardDynamicsContactsOld (
		Model &model,
		const VectorNd &Q,
		const VectorNd &QDot,
		const VectorNd &Tau,
		std::vector<ContactInfo> &ContactData,
		VectorNd &QDDot
		) {
	LOG << "-------- " << __func__ << " ------" << std::endl;

//	LOG << "Q    = " << Q.transpose() << std::endl;
//	LOG << "QDot = " << QDot.transpose() << std::endl;

//	assert (ContactData.size() == 1);
	std::vector<SpatialVector> f_t (ContactData.size(), SpatialVectorZero);
	std::vector<SpatialVector> f_ext_constraints (model.mBodies.size(), SpatialVectorZero);
	std::vector<Vector3d> point_accel_0 (ContactData.size(), Vector3d::Zero());
	VectorNd QDDot_0 = VectorNd::Zero(model.dof_count);
	VectorNd QDDot_t = VectorNd::Zero(model.dof_count);

	MatrixNd K = MatrixNd::Zero(ContactData.size(), ContactData.size());
	VectorNd f = VectorNd::Zero(ContactData.size());
	VectorNd a = VectorNd::Zero(ContactData.size());

	Vector3d point_accel_t;

	unsigned int ci = 0;
	
	// The default acceleration only needs to be computed once
	{
		SUPPRESS_LOGGING;
		ForwardDynamics (model, Q, QDot, Tau, QDDot_0);
	}

	// The vector f_ext_constraints might contain some values from previous
	// computations so we need to reset it.
	for (unsigned int fi = 0; fi < f_ext_constraints.size(); fi++) {
			f_ext_constraints[fi].setZero();
		}

	// we have to compute the standard accelerations first as we use them to
	// compute the effects of each test force
	LOG << "=== Initial Loop Start ===" << std::endl;
	for (ci = 0; ci < ContactData.size(); ci++) {
		unsigned int body_id = ContactData[ci].body_id;
		Vector3d point = ContactData[ci].point;
		Vector3d normal = ContactData[ci].normal;
		double acceleration = ContactData[ci].acceleration;

		{
			SUPPRESS_LOGGING;
			UpdateKinematicsCustom (model, NULL, NULL, &QDDot_0);
			point_accel_0[ci] = CalcPointAcceleration (model, Q, QDot, QDDot_0, body_id, point, false);

			a[ci] = acceleration - ContactData[ci].normal.dot(point_accel_0[ci]);
		}
		LOG << "point_accel_0 = " << point_accel_0[ci].transpose();
	}

	// Now we can compute and apply the test forces and use their net effect
	// to compute the inverse articlated inertia to fill K.
	for (ci = 0; ci < ContactData.size(); ci++) {
		LOG << "=== Testforce Loop Start ===" << std::endl;
		unsigned int body_id = ContactData[ci].body_id;
		Vector3d point = ContactData[ci].point;
		Vector3d normal = ContactData[ci].normal;
		double acceleration = ContactData[ci].acceleration;

		// assemble the test force
		LOG << "normal = " << normal.transpose() << std::endl;

		Vector3d point_global = CalcBodyToBaseCoordinates (model, Q, body_id, point, false);
		LOG << "point_global = " << point_global.transpose() << std::endl;

		f_t[ci].set (0., 0., 0., -normal[0], -normal[1], -normal[2]);
		f_t[ci] = spatial_adjoint(Xtrans_mat(-point_global)) * f_t[ci];
		f_ext_constraints[body_id] = f_t[ci];

		LOG << "f_t[" << ci << "] (i = ci) = " << f_t[ci].transpose() << std::endl;
		LOG << "f_t[" << body_id << "] (i = body_id) = " << f_t[body_id].transpose() << std::endl;

		{
//			SUPPRESS_LOGGING;
//			ForwardDynamicsAccelerationsOnly (model, QDDot_t, &f_ext_constraints);
			ForwardDynamics (model, Q, QDot, Tau, QDDot_t, &f_ext_constraints);
			LOG << "QDDot_0 = " << QDDot_0.transpose() << std::endl;
			LOG << "QDDot_t = " << QDDot_t.transpose() << std::endl;
			LOG << "QDDot_t - QDDot_0= " << (QDDot_t - QDDot_0).transpose() << std::endl;
		}
		f_ext_constraints[body_id].setZero();

		// compute the resulting acceleration
		{
			SUPPRESS_LOGGING;
			UpdateKinematicsCustom (model, NULL, NULL, &QDDot_t);
		}

		for (unsigned int cj = 0; cj < ContactData.size(); cj++) {
			{
				SUPPRESS_LOGGING;

				point_accel_t = CalcPointAcceleration (model, Q, QDot, QDDot_t, ContactData[cj].body_id, ContactData[cj].point, false);
			}
	
			LOG << "point_accel_0  = " << point_accel_0[ci].transpose() << std::endl;
			K(cj,ci) = ContactData[cj].normal.dot(- point_accel_0[cj] + point_accel_t);
			LOG << "point_accel_t = " << point_accel_t.transpose() << std::endl;
		}
	}

	LOG << "K = " << std::endl << K << std::endl;
	LOG << "a = " << std::endl << a << std::endl;

#ifndef RBDL_USE_SIMPLE_MATH
//	f = K.ldlt().solve (a);
	f = K.colPivHouseholderQr().solve (a);
#else
	bool solve_successful = LinSolveGaussElimPivot (K, a, f);
	assert (solve_successful);
#endif

	LOG << "f = " << f << std::endl;

	for (unsigned int i = 0; i < f_ext_constraints.size(); i++) {
		f_ext_constraints[i].setZero();
	}

	for (ci = 0; ci < ContactData.size(); ci++) {
		ContactData[ci].force = f[ci];
		unsigned int body_id = ContactData[ci].body_id;

		f_ext_constraints[body_id] += f_t[ci] * f[ci]; 
		LOG << "f_ext[" << body_id << "] = " << f_ext_constraints[body_id].transpose() << std::endl;
	}

	{
		SUPPRESS_LOGGING;
//		ForwardDynamicsAccelerationsOnly (model, QDDot, &f_ext_constraints);
		ForwardDynamics (model, Q, QDot, Tau, QDDot, &f_ext_constraints);
	}
}

/** \brief Computes the effect of external forces on the generalized accelerations.
 *
 * This function is essentially similar to ForwardDynamics() except that it
 * tries to only perform computations of variables that change due to
 * external forces defined in f_t.
 */
void ForwardDynamicsAccelerationDeltas (
		Model &model,
		ConstraintSet &CS,
		VectorNd &QDDot_t,
		const unsigned int body_id,
		const std::vector<SpatialVector> &f_t
		) {
	LOG << "-------- " << __func__ << " ------" << std::endl;

	assert (CS.d_pA.size() == model.mBodies.size());
	assert (CS.d_a.size() == model.mBodies.size());
	assert (CS.d_u.size() == model.mBodies.size());

	// TODO reset all values (debug)
	for (unsigned int i = 0; i < model.mBodies.size(); i++) {
		CS.d_pA[i].setZero();
		CS.d_a[i].setZero();
		CS.d_u[i] = 0.;
	}

	for (unsigned int i = body_id; i > 0; i--) {
		if (i == body_id) {
			CS.d_pA[i] = -spatial_adjoint(model.X_base[i].toMatrix()) * f_t[i];
		}

		CS.d_u[i] = - model.S[i].dot(CS.d_pA[i]);

		unsigned int lambda = model.lambda[i];
		if (lambda != 0) {
			CS.d_pA[lambda] = CS.d_pA[lambda] + model.X_lambda[i].toMatrixTranspose() * (CS.d_pA[i] + model.U[i] * CS.d_u[i] / model.d[i]);
		}
	}

	for (unsigned int i = 0; i < f_t.size(); i++) {
		LOG << "f_t[" << i << "] = " << f_t[i].transpose();
	}

	for (unsigned int i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_pA[i] " << CS.d_pA[i].transpose();
	}
	for (unsigned int i = 0; i < model.mBodies.size(); i++) {
		LOG << "i = " << i << ": d_u[i] = " << CS.d_u[i] << std::endl;
	}

	QDDot_t[0] = 0.;
	CS.d_a[0] = model.a[0];

	for (unsigned int i = 1; i < model.mBodies.size(); i++) {
		unsigned int lambda = model.lambda[i];

		SpatialVector Xa = model.X_lambda[i].apply(CS.d_a[lambda]);
		QDDot_t[i - 1] = (CS.d_u[i] - model.U[i].dot(Xa) ) / model.d[i];
		CS.d_a[i] = Xa + model.S[i] * QDDot_t[i - 1];

		LOG << "QDDot_t[" << i - 1 << "] = " << QDDot_t[i - 1] << std::endl;
		LOG << "d_a[i] = " << CS.d_a[i].transpose() << std::endl;
	}
}

inline void set_zero (std::vector<SpatialVector> &spatial_values) {
	for (unsigned int i = 0; i < spatial_values.size(); i++)
		spatial_values[i].setZero();
}

void ForwardDynamicsContacts (
		Model &model,
		const VectorNd &Q,
		const VectorNd &QDot,
		const VectorNd &Tau,
		ConstraintSet &CS,
		VectorNd &QDDot
		) {
	LOG << "-------- " << __func__ << " ------" << std::endl;

//	LOG << "Q    = " << Q.transpose() << std::endl;
//	LOG << "QDot = " << QDot.transpose() << std::endl;
//	assert (ContactData.size() == 1);

	assert (CS.f_ext_constraints.size() == model.mBodies.size());
	assert (CS.QDDot_0.size() == model.dof_count);
	assert (CS.QDDot_t.size() == model.dof_count);
	assert (CS.f_t.size() == CS.size());
	assert (CS.point_accel_0.size() == CS.size());
	assert (CS.K.rows() == CS.size());
	assert (CS.K.cols() == CS.size());
	assert (CS.constraint_force.size() == CS.size());
	assert (CS.a.size() == CS.size());

	Vector3d point_accel_t;

	unsigned int ci = 0;
	
	// The default acceleration only needs to be computed once
	{
		SUPPRESS_LOGGING;
		ForwardDynamics (model, Q, QDot, Tau, CS.QDDot_0);
	}

	// The vector f_ext_constraints might contain some values from previous
	// computations so we need to reset it.
	for (unsigned int fi = 0; fi < CS.f_ext_constraints.size(); fi++) {
//			CS.f_ext_constraints[fi].setZero();
	}

	LOG << "=== Initial Loop Start ===" << std::endl;
	// we have to compute the standard accelerations first as we use them to
	// compute the effects of each test force
	for (ci = 0; ci < CS.size(); ci++) {
		unsigned int body_id = CS.body[ci];
		Vector3d point = CS.point[ci];
		Vector3d normal = CS.normal[ci];
		double acceleration = CS.constraint_acceleration[ci];

		LOG << "body_id = " << body_id << std::endl;
		LOG << "point = " << point << std::endl;
		LOG << "normal = " << normal << std::endl;
		LOG << "QDDot_0 = " << CS.QDDot_0.transpose() << std::endl;
		{
			SUPPRESS_LOGGING;
			UpdateKinematicsCustom (model, NULL, NULL, &CS.QDDot_0);
			CS.point_accel_0[ci] = CalcPointAcceleration (model, Q, QDot, CS.QDDot_0, body_id, point, false);

			CS.a[ci] = acceleration - normal.dot(CS.point_accel_0[ci]);
		}
		LOG << "point_accel_0 = " << CS.point_accel_0[ci].transpose();
	}

	// Now we can compute and apply the test forces and use their net effect
	// to compute the inverse articlated inertia to fill K.
	for (ci = 0; ci < CS.size(); ci++) {
		LOG << "=== Testforce Loop Start ===" << std::endl;
		unsigned int body_id = CS.body[ci];
		Vector3d point = CS.point[ci];
		Vector3d normal = CS.normal[ci];
		double acceleration = CS.constraint_acceleration[ci];

		// assemble the test force
		LOG << "normal = " << normal.transpose() << std::endl;

		Vector3d point_global = CalcBodyToBaseCoordinates (model, Q, body_id, point, false);
		LOG << "point_global = " << point_global.transpose() << std::endl;

		CS.f_t[ci].set (0., 0., 0., -normal[0], -normal[1], -normal[2]);
		CS.f_t[ci] = spatial_adjoint(Xtrans_mat(-point_global)) * CS.f_t[ci];
		CS.f_ext_constraints[body_id] = CS.f_t[ci];
		LOG << "f_t[" << body_id << "] = " << CS.f_t[ci].transpose() << std::endl;

		{
//			SUPPRESS_LOGGING;
			ForwardDynamicsAccelerationDeltas (model, CS, CS.QDDot_t, body_id, CS.f_ext_constraints);
			LOG << "QDDot_0 = " << CS.QDDot_0.transpose() << std::endl;
			LOG << "QDDot_t = " << (CS.QDDot_t + CS.QDDot_0).transpose() << std::endl;
			LOG << "QDDot_t - QDDot_0= " << (CS.QDDot_t).transpose() << std::endl;
		}
		CS.f_ext_constraints[body_id].setZero();

		////////////////////////////
		CS.QDDot_t += CS.QDDot_0;

		// compute the resulting acceleration
		{
			SUPPRESS_LOGGING;
			UpdateKinematicsCustom (model, NULL, NULL, &CS.QDDot_t);
		}

		for (unsigned int cj = 0; cj < CS.size(); cj++) {
			{
				SUPPRESS_LOGGING;

				point_accel_t = CalcPointAcceleration (model, Q, QDot, CS.QDDot_t, CS.body[cj], CS.point[cj], false);
			}
	
			LOG << "point_accel_0  = " << CS.point_accel_0[ci].transpose() << std::endl;
			CS.K(ci,cj) = CS.normal[cj].dot(point_accel_t - CS.point_accel_0[cj]);
			LOG << "point_accel_t = " << point_accel_t.transpose() << std::endl;
		}
		//////////////////////////////////

		/*
		// update the spatial accelerations due to the test force
		for (unsigned j = 1; j < model.mBodies.size(); j++) {
			if (model.lambda[j] != 0) {
				model.a[j] = model.X_lambda[j] * model.a[model.lambda[j]] + model.c[j];
			}	else {
				model.a[j].setZero();
			}

			model.a[j] = model.a[j] + model.S[j] * QDDot_t[j - 1];
		}
		*/

		/////////////i
		/*

		QDDot_t += QDDot_0;

		{
			SUPPRESS_LOGGING;
			UpdateKinematicsCustom (model, NULL, NULL, &QDDot_t);
		}
		
		LOG << "SUUUHUUM = " << (QDDot_t).transpose() << std::endl;
	
		for (unsigned int cj = 0; cj < ContactData.size(); cj++) {
			static SpatialVector point_spatial_acc;
			{
				SUPPRESS_LOGGING;

				// computation of the net effect (acceleration due to the test
				// force)

				// method 1: simply compute the acceleration by calling
				// CalcPointAcceleration() (slow)
				point_accel_t = CalcPointAcceleration (model, Q, QDot, QDDot_t, ContactData[cj].body_id, ContactData[cj].point, false);

				// method 2: transforming the spatial acceleration
				// appropriately.(faster: 
//				point_global = CalcBodyToBaseCoordinates (model, Q, ContactData[cj].body_id, ContactData[cj].point, false);
//				point_spatial_acc = Xtrans (point_global) * (spatial_inverse(model.X_base[ContactData[cj].body_id]) * model.a[ContactData[cj].body_id]);
//				point_accel_t.set (point_spatial_acc[3], point_spatial_acc[4], point_spatial_acc[5]);

				// method 3: reduce 1 Matrix-Matrix computation:
				// \todo currently broken!
//				point_global = CalcBodyToBaseCoordinates (model, Q, ContactData[cj].body_id, ContactData[cj].point, false);
//				point_spatial_acc = spatial_inverse(model.X_base[ContactData[cj].body_id]) * model.a[ContactData[cj].body_id];
//
//				Matrix3d rx (0., point_global[2], -point_global[1], -point_global[2], 0, point_global[0], point_global[1], -point_global[0], 0.);
//				Matrix3d R = model.X_base[ContactData[cj].body_id].block<3,3>(0,0).transpose();
//				point_accel_t = rx * R * Vector3d (point_spatial_acc[0], point_spatial_acc[1], point_spatial_acc[2]) + Vector3d(point_spatial_acc[3], point_spatial_acc[4], point_spatial_acc[5]) ;
			}

			LOG << "point_spatial_a= " << point_spatial_acc.transpose() << std::endl;
			LOG << "point_accel_0  = [" << point_accel_0.transpose() << " ]" << std::endl;
			K(ci,cj) = ContactData[cj].normal.dot(point_accel_t);
			LOG << "point_accel_t = [" << (point_accel_t).transpose() << " ]" << std::endl;
		}
		*/
		////////////////
	}

	LOG << "K = " << std::endl << CS.K << std::endl;
	LOG << "a = " << std::endl << CS.a << std::endl;

#ifndef RBDL_USE_SIMPLE_MATH
	switch (CS.linear_solver) {
		case (ConstraintSet::LinearSolverPartialPivLU) :
			CS.constraint_force = CS.K.partialPivLu().solve(CS.a);
			break;
		case (ConstraintSet::LinearSolverColPivHouseholderQR) :
			CS.constraint_force = CS.K.colPivHouseholderQr().solve(CS.a);
			break;
		default:
			LOG << "Error: Invalid linear solver: " << CS.linear_solver << std::endl;
			assert (0);
			break;
	}
#else
	bool solve_successful = LinSolveGaussElimPivot (CS.K, CS.a, CS.constraint_force);
	assert (solve_successful);
#endif

	LOG << "f = " << CS.constraint_force.transpose() << std::endl;

	for (ci = 0; ci < CS.size(); ci++) {
		unsigned int body_id = CS.body[ci];

		CS.f_ext_constraints[body_id] += CS.f_t[ci] * CS.constraint_force[ci]; 
		LOG << "f_ext[" << body_id << "] = " << CS.f_ext_constraints[body_id].transpose() << std::endl;
	}

	{
		SUPPRESS_LOGGING;
		ForwardDynamicsApplyConstraintForces (model, CS, QDDot);
	}
}

} /* namespace RigidBodyDynamics */
