/*
 * intr.h
 *
 *  Created on: 2011/05/06
 *      Author: mizu
 */

#ifndef INTR_H_
#define INTR_H_

//#define SOFTVEC_TYPE_NUM		3

#define SOFTVEC_TYPE_SOFTERR	0
#define SOFTVEC_TYPE_SYSTEM		1
#define SOFTVEC_TYPE_SERIAL		2

//void interrupt(softvec_type_t type, unsigned long sp);

//typedef enum
//{
//	SOFTVEC_TYPE_SOFTERR = 0,
//	SOFTVEC_TYPE_SYSTEM,
//	SOFTVEC_TYPE_SERIAL
//}softvec_type_t;


#endif /* INTR_H_ */
