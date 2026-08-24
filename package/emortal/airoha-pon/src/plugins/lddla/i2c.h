#ifndef _I2C_H_
#define _I2C_H_

// ----- Include ----- 



// ----- Function ----- 
ushort lddla_I2C_read(unchar u1CHannelID, ushort u2ClkDiv, unchar u1DevAddr, 
				 unchar u1WordAddrNum, uint u4WordAddr, unchar *pu1Buf, 
				 ushort u2ByteCnt);
ushort lddla_I2C_write(unchar u1CHannelID, ushort u2ClkDiv, unchar u1DevAddr, 
						  unchar u1WordAddrNum, uint u4WordAddr, unchar *pu1Buf, 
						  ushort u2ByteCnt);


#endif /* _I2C_H_ */




