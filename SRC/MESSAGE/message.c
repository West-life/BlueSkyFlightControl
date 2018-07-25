/**********************************************************************************************************
                                天穹飞控 —— 致力于打造中国最好的多旋翼开源飞控
                                Github: github.com/loveuav/BlueSkyFlightControl
                                技术讨论：bbs.loveuav.com/forum-68-1.html
 * @文件     message.c
 * @说明     飞控数据通信，所有通信采用小端模式。通信协议自适应，根据第一次接收到的数据帧决定使用bsklink或mavlink
 * @版本  	 V1.1
 * @作者     BlueSky
 * @网站     bbs.loveuav.com
 * @日期     2018.07
**********************************************************************************************************/
#include "message.h"
#include "bsklinkSend.h"
#include "bsklinkDecode.h"
#include "mavlinkSend.h"
#include "mavlinkDecode.h"
#include "drv_usart.h"
#include "drv_usb.h"
#include "bsklink.h"
#include "common/mavlink.h"

#include "ahrs.h"
#include "flightControl.h"
#include "gyroscope.h"
#include "navigation.h"
#include "accelerometer.h"
#include "barometer.h"
#include "motor.h"
#include "gps.h"

//通信协议类型
enum MESSAGE_TYPE
{
    UNKNOWN = 0,
    BSKLINK = 1,
    MAVLINK = 2
};

/*
mavlink协议参考
http://mavlink.org/messages/common
https://mavlink.io/en/messages/common.html
*/

//mavlink的心跳包ID为0，无法参与发送列表排序，故为其重新定义一个ID（仅用于参与排序）
#define MAVLINK_MSG_ID_HEARTBEAT2   180     

uint8_t bsklinkSendFlag[0xFF];	            //发送标志位
uint8_t bsklinkSendFreq[0xFF];	            //发送频率
uint8_t bsklinkSortResult[0xFF];            //发送频率排序
uint8_t bsklinkSendList[MAX_SEND_FREQ];     //发送列表

uint8_t mavlinkSendFlag[0xFF];	            //发送标志位
uint8_t mavlinkSendFreq[0xFF];	            //发送频率
uint8_t mavlinkSortResult[0xFF];            //发送频率排序
uint8_t mavlinkSendList[MAX_SEND_FREQ];     //发送列表

//定义通信协议类型，根据接收到的数据帧进行自动检测
enum MESSAGE_TYPE messageType = UNKNOWN;

BSKLINK_PAYLOAD_SENSOR_CALI_CMD_t sensorCali;

static void MessageProtocolTypeDetect(uint8_t data);
static void SendFreqSort(uint8_t* sortResult, uint8_t* sendFreq);
static void SendListCreate(uint8_t* sendFreq, uint8_t* sortResult, uint8_t* sendList);

/**********************************************************************************************************
*函 数 名: MessageInit
*功能说明: 飞控数据通信初始化
*形    参: 无
*返 回 值: 无
**********************************************************************************************************/
void MessageInit(void)
{
    //初始化通信协议检测函数为数据接收中断回调函数
    Usart_SetIRQCallback(DATA_UART, MessageProtocolTypeDetect);
    Usb_SetRecvCallback(MessageProtocolTypeDetect);
    
    /*初始化各帧的发送频率，各帧频率和不能超过MAX_SEND_FREQ*/
    //bsklink发送频率
    bsklinkSendFreq[BSKLINK_MSG_ID_FLIGHT_DATA]        = 15;
    bsklinkSendFreq[BSKLINK_MSG_ID_SENSOR]             = 5; 
    bsklinkSendFreq[BSKLINK_MSG_ID_SENSOR_CALI_DATA]   = 1; 
    bsklinkSendFreq[BSKLINK_MSG_ID_RC_DATA]            = 5; 
	bsklinkSendFreq[BSKLINK_MSG_ID_MOTOR]              = 0; 
    bsklinkSendFreq[BSKLINK_MSG_ID_FLIGHT_STATUS]      = 1;
    bsklinkSendFreq[BSKLINK_MSG_ID_GPS]                = 2; 
    bsklinkSendFreq[BSKLINK_MSG_ID_BATTERY]            = 1;  
    bsklinkSendFreq[BSKLINK_MSG_ID_HEARTBEAT]          = 1;     //心跳包发送频率为固定1Hz
    //mavlink发送频率
    mavlinkSendFreq[MAVLINK_MSG_ID_SYS_STATUS]         = 1;
    mavlinkSendFreq[MAVLINK_MSG_ID_GPS_RAW_INT]        = 1;
    mavlinkSendFreq[MAVLINK_MSG_ID_ATTITUDE]           = 15;
    mavlinkSendFreq[MAVLINK_MSG_ID_SCALED_IMU]         = 5;
    mavlinkSendFreq[MAVLINK_MSG_ID_HEARTBEAT2]         = 1;     //心跳包发送频率为固定1Hz
    
    //生成bsklink发送列表
    SendFreqSort(bsklinkSortResult, bsklinkSendFreq);
    SendListCreate(bsklinkSendFreq, bsklinkSortResult, bsklinkSendList);
    //生成mavlink发送列表    
    SendFreqSort(mavlinkSortResult, mavlinkSendFreq);
    SendListCreate(mavlinkSendFreq, mavlinkSortResult, mavlinkSendList);
}

/**********************************************************************************************************
*函 数 名: MessageSendLoop
*功能说明: 检测是否有需要发送的数据帧
*形    参: 无
*返 回 值: 无
**********************************************************************************************************/
void MessageSendLoop(void)
{    
    static uint32_t i = 0;
    
    if(messageType == BSKLINK)
    {
        //根据需求发送的数据帧
        if(bsklinkSendFlag[BSKLINK_MSG_ID_SENSOR_CALI_CMD] == ENABLE) 					   //传感器校准反馈 
            BsklinkSendSensorCaliCmd(&bsklinkSendFlag[BSKLINK_MSG_ID_SENSOR_CALI_CMD], sensorCali.type, sensorCali.step, sensorCali.successFlag);                  
        else if(bsklinkSendFlag[BSKLINK_MSG_ID_PID_ATT] == ENABLE)
            BsklinkSendPidAtt(&bsklinkSendFlag[BSKLINK_MSG_ID_PID_ATT]);                   //姿态PID    
        else if(bsklinkSendFlag[BSKLINK_MSG_ID_PID_POS] == ENABLE)
            BsklinkSendPidPos(&bsklinkSendFlag[BSKLINK_MSG_ID_PID_POS]);                   //位置PID
        //循环发送的数据
        else
        {
            //根据发送列表来使能对应的数据帧发送标志位
            bsklinkSendFlag[bsklinkSendList[(i++) % MAX_SEND_FREQ]] = ENABLE; 
            
            BsklinkSendFlightData(&bsklinkSendFlag[BSKLINK_MSG_ID_FLIGHT_DATA]);           //基本飞行数据
            BsklinkSendFlightStatus(&bsklinkSendFlag[BSKLINK_MSG_ID_FLIGHT_STATUS]);       //飞行状态信息
            BsklinkSendSensor(&bsklinkSendFlag[BSKLINK_MSG_ID_SENSOR]);                    //传感器数据
            BsklinkSendSensorCaliData(&bsklinkSendFlag[BSKLINK_MSG_ID_SENSOR_CALI_DATA]);  //传感器校准数据
            BsklinkSendRcData(&bsklinkSendFlag[BSKLINK_MSG_ID_RC_DATA]);                   //遥控通道数据
            BsklinkSendMotor(&bsklinkSendFlag[BSKLINK_MSG_ID_MOTOR]);					   //电机输出
            BsklinkSendGps(&bsklinkSendFlag[BSKLINK_MSG_ID_GPS]);                          //GPS数据
            BsklinkSendHeartBeat(&bsklinkSendFlag[BSKLINK_MSG_ID_HEARTBEAT]);              //心跳包
        }
    }
    else if(messageType == MAVLINK)
    {
        if(mavlinkSendFlag[MAVLINK_MSG_ID_PARAM_VALUE])
            MavlinkSendParamValue(&mavlinkSendFlag[MAVLINK_MSG_ID_PARAM_VALUE]);               //飞控参数
        else
        {
            //根据发送列表来使能对应的数据帧发送标志位
            mavlinkSendFlag[mavlinkSendList[(i++) % MAX_SEND_FREQ]] = ENABLE;  
            
            MavlinkSendSysStatus(&mavlinkSendFlag[MAVLINK_MSG_ID_SYS_STATUS]);                 //系统状态
            MavlinkSendGpsRawInt(&mavlinkSendFlag[MAVLINK_MSG_ID_GPS_RAW_INT]);                //GPS原始数据
            MavlinkSendAttitude(&mavlinkSendFlag[MAVLINK_MSG_ID_ATTITUDE]);                    //姿态角度和角速度
            MavlinkSendScaledImu(&mavlinkSendFlag[MAVLINK_MSG_ID_SCALED_IMU]);                 //IMU原始数据
            MavlinkSendHeartbeat(&mavlinkSendFlag[MAVLINK_MSG_ID_HEARTBEAT2]);                 //心跳包
        }
    }
}

/**********************************************************************************************************
*函 数 名: MessageProtocolTypeDetect
*功能说明: 通信协议检测
*形    参: 接收数据
*返 回 值: 无
**********************************************************************************************************/
static void MessageProtocolTypeDetect(uint8_t data)
{
    static BSKLINK_MSG_t bskMsg;
    static mavlink_message_t mavMsg;
    static mavlink_status_t  mavStatus;  
  
    //检测bsklink协议
    if(BsklinkParseChar(&bskMsg, data) == true)  
    {
        //设置协议类型为bsklink
        messageType = BSKLINK;
        
        //重新设置数据接收中断回调函数
        Usart_SetIRQCallback(DATA_UART, BsklinkDecode);
        Usb_SetRecvCallback(BsklinkDecode);
    }  

    //检测mavlink协议
    if(mavlink_parse_char(0, data, &mavMsg, &mavStatus) == true)
    {
        //设置协议类型为mavlink
        messageType = MAVLINK;
        
        //重新设置数据接收中断回调函数
        Usart_SetIRQCallback(DATA_UART, MavlinkDecode);
        Usb_SetRecvCallback(MavlinkDecode);  
    }
}

/**********************************************************************************************************
*函 数 名: MessageSetSensorCaliFeedback
*功能说明: 传感器校准反馈消息发送使能
*形    参: 无
*返 回 值: 无
**********************************************************************************************************/
void MessageSensorCaliFeedbackEnable(uint8_t type, uint8_t step, uint8_t success)
{
	bsklinkSendFlag[BSKLINK_MSG_ID_SENSOR_CALI_CMD] = ENABLE;
	
	sensorCali.type = type;
	sensorCali.successFlag = success;
	sensorCali.step = step;
}

/**********************************************************************************************************
*函 数 名: BsklinkSendEnable
*功能说明: bsklink消息发送使能
*形    参: 消息ID
*返 回 值: 无
**********************************************************************************************************/
void BsklinkSendEnable(uint8_t msgid)
{
    bsklinkSendFlag[msgid] = ENABLE;
}

/**********************************************************************************************************
*函 数 名: MavlinkSendEnable
*功能说明: mavlink消息发送使能
*形    参: 消息ID
*返 回 值: 无
**********************************************************************************************************/
void MavlinkSendEnable(uint8_t msgid)
{
    mavlinkSendFlag[msgid] = ENABLE;
}

/**********************************************************************************************************
*函 数 名: SendFreqSort
*功能说明: 根据消息发送频率来给消息ID排序
*形    参: 排序结果数组指针 发送频率数组指针
*返 回 值: 无
**********************************************************************************************************/
static void SendFreqSort(uint8_t* sortResult, uint8_t* sendFreq)
{
    uint8_t i = 0, j = 0;
    uint8_t temp;
    
    //先初始化消息ID排序序列
    for(i = 0; i<0xFF; i++)
        sortResult[i] = i;

    //开始按照发送频率来给消息ID排序
    for(i=0; i<0xFF; i++)
    {    
        for(j=i+1; j<0xFF; j++)
        {
            if(sendFreq[sortResult[j]] > sendFreq[sortResult[i]])
            {
                temp = sortResult[i];
                sortResult[i] = sortResult[j];
                sortResult[j] = temp;
            }
        }
    }
    
    //除了第一个，其它改为倒序，目的是为了让所有数据帧发送尽可能均匀
    uint8_t validNum = 0;
    for(i=0; i<0xFF; i++)
    {
        if(sendFreq[sortResult[i]] != 0)
            validNum++;        
    }   
    
    validNum -= 1;
    
    for(i=1; i<=validNum/2; i++)
    {
        temp = sortResult[i];
        sortResult[i] = sortResult[validNum + 1 - i];
        sortResult[validNum + 1 - i] = temp;
    }
}

/**********************************************************************************************************
*函 数 名: SendListCreate
*功能说明: 根据各消息帧的发送频率自动生成发送列表
*形    参: 发送频率数组指针 排序结果数组指针 发送列表数组指针
*返 回 值: 无
**********************************************************************************************************/
static void SendListCreate(uint8_t* sendFreq, uint8_t* sortResult, uint8_t* sendList)
{
    uint8_t sendNum = 0;
    uint8_t i, j;
    static float interval;
    uint8_t random;
    
    //判断总发送量是否超出最大发送频率，若超过则退出该函数
    for(i=0; i<0xFF; i++)
    {
        if(sendFreq[sortResult[i]] == 0)
            break;
        
        sendNum += sendFreq[sortResult[i]];
    }
    if(sendNum > MAX_SEND_FREQ)
        return;
    
    //开始生成发送列表
    for(i=0; i<0xFF; i++)
    {
        if(sendFreq[sortResult[i]] == 0)
            return;

        //发送间隔
        interval = (float)MAX_SEND_FREQ / sendFreq[sortResult[i]];
		//生成随机数，作为该帧数据在列表中的排序起始点，这样可以尽量使各帧数据分布均匀
        random   = GetRandom() % MAX_SEND_FREQ;
		
        for(j=0; j<sendFreq[sortResult[i]]; j++)
        {
            for(uint8_t k=0; k<MAX_SEND_FREQ-j*interval; k++)
            {
                if(sendList[(int16_t)(j*interval+k+random) % MAX_SEND_FREQ] == 0)
                {
                    sendList[(int16_t)(j*interval+k+random) % MAX_SEND_FREQ] = sortResult[i];
                    break;
                }
            }
        }
    }
}

/**********************************************************************************************************
*函 数 名: DataSend
*功能说明: 数据发送接口
*形    参: 数据指针 长度
*返 回 值: 无
**********************************************************************************************************/
void DataSend(uint8_t *data , uint8_t length)
{
    //串口发送
    Usart_SendData(DATA_UART, data, length);
    
    //USB转串口发送
    Usb_Send(data, length);
}



