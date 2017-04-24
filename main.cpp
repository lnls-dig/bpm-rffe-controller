#include "pcbnAPI.h"
#include "mbed.h"
#include "rtos.h"
#include "EthernetInterface.h"
#include "USBHostSerial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "PID.h"
#include "Drivers.h"
#include "cli.h"

#include "lpc_phy.h"
extern "C" {
#include "server.h"
#include <bsmp/server.h>
}


#define DP8_SPEED10MBPS (1 << 1)    /**< 1=10MBps speed */
#define DP8_VALID_LINK  (1 << 0)    /**< 1=Link active */

#define BUFSIZE         140
#define SERVER_PORT     6791

// Constants
#define PID_RATE        1.0
#define PID_OUTMAX      3.3
#define PID_OUTMIN      0.0

#define FILE_DATASIZE   127

#define FW_VERSION      "V2_0005"

extern "C" void mbed_reset();

// BSMP Variables arrays
double Att[1];
double TempAC[1];
double TempBD[1];
double Set_PointAC[1];
double Set_PointBD[1];
int Temp_Control[1];
double HeaterAC[1];
double HeaterBD[1];
int Reset[1];
int Reprogramming[1];
uint8_t Data[FILE_DATASIZE];
char Version[8];
double PID_AC_Kc[1];
double PID_AC_tauI[1];
double PID_AC_tauD[1];
double PID_BD_Kc[1];
double PID_BD_tauI[1];
double PID_BD_tauD[1];

#define READ_ONLY  0
#define READ_WRITE 1

#define RFFE_VAR( data, rw ) { { 0, rw, sizeof(data) }, NULL, data, NULL }
/* The index in this table will coincide with the index on the server list, since it registrates the variables sequentially */
struct bsmp_var rffe_vars[] = {
    [0]  = RFFE_VAR( Att,            READ_WRITE ), // Attenuators
    [1]  = RFFE_VAR( TempAC,         READ_ONLY ), // TempAC
    [2]  = RFFE_VAR( TempBD,         READ_ONLY ), // TempBD
    [3]  = RFFE_VAR( Set_PointAC,    READ_WRITE ), // Set_PointAC
    [4]  = RFFE_VAR( Set_PointBD,    READ_WRITE ), // Set_PointBD
    [5]  = RFFE_VAR( Temp_Control,   READ_WRITE ), // Temp_Control
    [6]  = RFFE_VAR( HeaterAC,       READ_WRITE ), // HeaterAC
    [7]  = RFFE_VAR( HeaterBD,       READ_WRITE ), // HeaterBD
    [8]  = RFFE_VAR( Reset,          READ_WRITE ), // Reset
    [9]  = RFFE_VAR( Reprogramming,  READ_WRITE ), // Reprogramming
    [10] = RFFE_VAR( Data,           READ_WRITE ), // Data
    [11] = RFFE_VAR( Version,        READ_ONLY ), // Version
    [12] = RFFE_VAR( PID_AC_Kc,      READ_WRITE ), // PID_AC_Kc
    [13] = RFFE_VAR( PID_AC_tauI,    READ_WRITE ), // PID_AC_tauI
    [14] = RFFE_VAR( PID_AC_tauD,    READ_WRITE ), // PID_AC_tauD
    [15] = RFFE_VAR( PID_BD_Kc,      READ_WRITE ), // PID_BD_Kc
    [16] = RFFE_VAR( PID_BD_tauI,    READ_WRITE ), // PID_BD_tauI
    [17] = RFFE_VAR( PID_BD_tauD,    READ_WRITE ), // PID_BD_tauD
};

// Create the local filesystem under the name "local"
LocalFileSystem localdir("local");
FILE *fp;

char rffe_ip[16];
char rffe_mac[18];

// Hardware Initialization - MBED

// MBED Leds
DigitalOut led1(LED1);
DigitalOut led2(LED2);
DigitalOut led3(LED3);
DigitalOut led4(LED4);

// MBED pins

DigitalOut dataC(p10); // Data line to attenuator. LVTTL, low = reset, init = low.Set first attenuators (Att)
DigitalOut dataA(p11); // Data line to attenuator. LVTTL, low = reset, init = low.Set first attenuators (Att)
DigitalOut clk(p12); // Digital control attenuation. LVTTL, low = reset, init = low.Digital control attenuation
DigitalOut LE(p13); // Chip select for RFFE attenuators (all channels). LVTTL, low = reset, init = low.Digital control calibration
DigitalOut CSac(p25); // Chip select for ADT7320UCPZ-R2. LVTTL, high = disable, init = high.Temp. measurement in RFFE_AC
DigitalOut SHDN_temp(p15); // Shut down the temperature current boost output amplifier. LVTTL, low = disable, init = low.
DigitalOut led_g(p19); // Green LED
DigitalOut led_r(p20); // Red LED
DigitalOut CSbd(p14); // Chip select for ADT7320UCPZ-R2. LVTTL, high = disable, init = high.Temp. measurement in RFFE_BD
DigitalOut dataB(p26); // Data line to attenuator. LVTTL, low = reset, init = low. Set RF attenuators (Att)
DigitalOut dataD(p27); // Data line to attenuator. LVTTL, low = reset, init = low. Set RF attenuators (Att)
DigitalOut CS_dac(p16); // Chip select for DAC. LVTTL, low = Selected, init = high.Chip select
DigitalOut LedY(p29); // Yellow led of the Ethernet connector. LVTTLIndicate active connection
DigitalOut LedG(p30); // Green led of the Ethernet connector. LVTTLIndicate transmiting data
Serial pc(USBTX, USBRX); // Serial USB port. (NOTE: All printf() calls are redirected to this port)
SPI spi1(p5,p6,p7); //SPI Interface - spi(mosi, miso, sclk)

bool get_eth_link_status(void)
{
    return (lpc_mii_read_data() & DP8_VALID_LINK) ? true : false;
}

//********************************************** Thread functions **********************************************************
void Temp_Feedback_Control(void const *args)
{
    // Init. config
    double SetP_AC, SetP_BD;
    double ProcessValueAC, ProcessValueBD;
    double voutAC, voutBD;

    int state = 2;
    int pid_state = MANUAL;

    /* Temperature Sensors */
    ADT7320 AC_Temp_sensor( spi1, CSac, 1000000, ADT7320_CFG_16_BITS, 0, 0.0, 100.0 );
    ADT7320 BD_Temp_sensor( spi1, CSbd, 1000000, ADT7320_CFG_16_BITS, 0, 0.0, 100.0 );

    DAC7554 AC_Heater_DAC( spi1, CS_dac, DAC_AC_SEL, 3.3 );
    DAC7554 BD_Heater_DAC( spi1, CS_dac, DAC_BD_SEL, 3.3 );

    /* Create PIDs with generic tuning constants (they will be updated as soon as the control loop starts) */
    PID pidAC( &ProcessValueAC, &voutAC, &SetP_AC, get_value64(PID_AC_Kc), get_value64(PID_AC_tauI), get_value64(PID_AC_tauD), DIRECT );
    pidAC.SetSampleTime( PID_RATE*1000 );
    //pidAC.SetInputLimits( 0.0 , 100.0 );
    pidAC.SetOutputLimits( PID_OUTMIN , PID_OUTMAX );
    pidAC.SetMode( pid_state ); // Start with the automatic control disabled

    PID pidBD( &ProcessValueBD, &voutBD, &SetP_BD, get_value64(PID_BD_Kc), get_value64(PID_BD_tauI), get_value64(PID_BD_tauD), DIRECT );
    pidBD.SetSampleTime( PID_RATE*1000 );
    //pidBD.SetInputLimits( 0.0 , 100.0 );
    pidBD.SetOutputLimits( PID_OUTMIN, PID_OUTMAX );
    pidAC.SetMode( pid_state ); // Start with the automatic control disabled

    SHDN_temp = 1;

    while (1) {

        if (state != get_value32(Temp_Control)) {
            printf ("New temp_control state : %s\n", get_value32(Temp_Control) ? "AUTOMATIC":"MANUAL");
            state = get_value32(Temp_Control);

            pid_state = (state != MANUAL) ? AUTOMATIC : MANUAL;
        }

        pidAC.SetMode( pid_state );
        pidBD.SetMode( pid_state );

        // Read temp from ADT7320 in RFFEs
        set_value(TempAC,AC_Temp_sensor.Read());
        set_value(TempBD,BD_Temp_sensor.Read());

        // Update the Process Values
        ProcessValueAC = get_value64(TempAC);
        ProcessValueBD = get_value64(TempBD);

        /* Update Set Points */
        SetP_AC = get_value64(Set_PointAC);
        SetP_BD = get_value64(Set_PointBD);

#ifdef DEBUG_PRINTF
        printf( "AC_Temp = %f \n", ProcessValueAC );
        printf( "BD_Temp = %f \n", ProcessValueBD );
        printf( "PID_AC Params:\n");
        printf( "\tKc:%f\ttauI:%f\ttauD:%f\n", get_value64(PID_AC_Kc), get_value64(PID_AC_tauI), get_value64(PID_AC_tauD));
        printf( "PID_BD Params:\n");
        printf( "\tKc:%f\ttauI:%f\ttauD:%f\n", get_value64(PID_BD_Kc), get_value64(PID_BD_tauI), get_value64(PID_BD_tauD));
#endif

        // Update PID tuning values
        pidAC.SetTunings( get_value64(PID_AC_Kc), get_value64(PID_AC_tauI), get_value64(PID_AC_tauD) );
        pidBD.SetTunings( get_value64(PID_BD_Kc), get_value64(PID_BD_tauI), get_value64(PID_BD_tauD) );

        /* Compute the PID values (this functions returns false and does nothing if set in MANUAL mode) */
        if ( ( pidAC.Compute() == false ) ) {
            // Use the heater values provided by the user
            voutAC = get_value64(HeaterAC);

            // Check if the user set values within the DAC range
            if (get_value64(HeaterAC) > PID_OUTMAX) {
                voutAC = PID_OUTMAX;
            }
            if (get_value64(HeaterAC) < PID_OUTMIN) {
                voutAC = PID_OUTMIN;
            }
        }

        if ( ( pidBD.Compute() == false ) ) {
            // Use the heater values provided by the user
            voutBD = get_value64(HeaterBD);

            // Check if the user set values within the DAC range
            if (get_value64(HeaterBD) > PID_OUTMAX) {
                voutBD = PID_OUTMAX;
            }
            if (get_value64(HeaterBD) < PID_OUTMIN) {
                voutBD = PID_OUTMIN;
            }
        }

        // Update values in BSMP registers list
        set_value(HeaterAC, voutAC);
        set_value(HeaterBD, voutBD);

        AC_Heater_DAC.Write( voutAC );
        BD_Heater_DAC.Write( voutBD );

#ifdef DEBUG_PRINTF
        printf("Heater output AC: %f \t BD: %f\n", voutAC, voutBD);
#endif
        Thread::wait(int(1000*PID_RATE));
    }
}

void int2bin6(int value, bool * bin)
{
    int resul;
    bin[0] = 0;
    bin[1] = 0;
    bin[2] = 0;
    bin[3] = 0;
    bin[4] = 0;
    bin[5] = 0;
    for (int i = 5; i >= 0; i--) {
        bin[i] = 1;
        resul = bin[5]*32+bin[4]*16+bin[3]*8+bin[2]*4+bin[1]*2+bin[0]*1;
        if (resul == value) return;
        if (resul > value) bin[i] = 0;
    }
}

void Clk_att(void const *arg)
{
    clk = !clk;
}

void Attenuators_Control(void const *arg)
{
    double prev_att1 = get_value64(Att)+1;
    bool attVec1[6];
    RtosTimer Clock_thread(Clk_att);

    while (1) {

        // Attenuators set
        if ( prev_att1 != get_value64(Att) ) {
            // Checking and setting attenuators value to fisable values
            set_value(Att,(float)(int(get_value64(Att)*2))/2);

#ifdef DEBUG_PRINTF
            printf("\nAtt values updated from: %f to %f\n", prev_att1, get_value64(Att));
#endif
            // Updating previous values
            prev_att1 = get_value64(Att);
            int2bin6(int(prev_att1*2), attVec1);

            LE = 0;
            clk = 0;
            Clock_thread.start(10);
            // Serial data to attenuators
            for (int i = 5; i >= 0; i--) {
                dataA = attVec1[i];
                dataB = attVec1[i];
                dataC = attVec1[i];
                dataD = attVec1[i];
                while (clk == 0) Thread::wait(1);// Waiting clock change
                while (clk == 1) Thread::wait(1);// Waiting clock change
            }
            LE = 1;
            Thread::wait(1);
            LE = 0;
            Clock_thread.stop();
        }
        Thread::wait(1000);
    }
}

int check_name(char * name)
{
    if ((name[0] == 'V') && isdigit(name[1]) && (name[2] == '_') && isdigit(name[3]) && isdigit(name[4]) && isdigit(name[5]) && isdigit(name[6]))
        return ((name[3]-48)*1000+(name[2]-48)*100+(name[1]-48)*10+(name[0]-48));
    else
        return 0;
}

int str2int(char * str)
{
    return ((str[0]-48)*1000 + (str[1]-48)*100 + (str[2]-48)*10 + (str[3]-48));
}

void int2str(char * str, int num)
{
    str[0] = char(num/1000 + 48);
    num = num%1000;
    str[1] = char(num/100 + 48);
    num = num%100;
    str[2] = char(num/10 + 48);
    num = num%10;
    str[3] = num+48;
}

void Update_Software(char * old_name, char * name)
{
    char path[15] = "/local/";
    if (fopen((char*)strcat(path,name),"rb") == NULL) {
        printf("New firmare not found\n");
        return;
    }
    printf("Reprogramming...\n");
    strcpy(path,"/local/");
    remove((char*)strcat(path,old_name));
    printf("Reseting...\n");
    mbed_reset();

}

#if 0
void Data_Check( void )
{
    for (int i = 0; i < FILE_DATASIZE; i++) {
        if ((i < (FILE_DATASIZE-4)) && Data[i] == 13 && Data[i+1] == 13 && Data[i+2] == 13 && Data[i+3] == 13)
            break;
        fputc(Data[i], fp);
    }
}
#endif

char cli_cmd[SCMD_MAX_CMD_LEN+1];

void commandCallback(char *cmdIn, void *extraContext) {
    // all our commands will be recieved async in commandCallback
    // we don't want to do time consuming things since it could
    // block the reader and allow the uart to overflow so we simply
    // copy it out in the callback and then process it latter.

#ifdef DEBUG_PRINTF
    printf("CLI Command Received =%s\r\n", cli_cmd);
#endif
    strcpy(cli_cmd, cmdIn);

    Thread *CLI_Thread = static_cast<Thread *>(extraContext);
    CLI_Thread->signal_set(0x01);
}

void CLI_Proccess( void const * args)
{
    char *cmd, *save_ptr;
    char *arg[2];
    printf("Initializing CLI_Proccess thread\n");
    for( ; ; ) {
        Thread::signal_wait(0x01);
        cmd = strtok_r( cli_cmd, " ", &save_ptr);

        for ( uint8_t i = 0; i < sizeof(arg)/sizeof(arg[0]); i++) {
            arg[i] = strtok_r( NULL, " ", &save_ptr);
        }

        if (strcmp( cmd, "dump" ) == 0) {
            printf("RFFE Vars dump:\n");
            printf("\tIP-Address: %s\n", rffe_ip);
            printf("\tMAC-Address: %s\n", rffe_mac);
            printf("\t[0]  Att: %f\n", get_value64(Att));
            printf("\t[1]  Temperature AC: %f\n", get_value64(TempAC));
            printf("\t[2]  Temperature BD: %f\n", get_value64(TempBD));
            printf("\t[3]  Set PointAC: %f\n", get_value64(Set_PointAC));
            printf("\t[4]  Set PointBD: %f\n", get_value64(Set_PointBD));
            printf("\t[5]  Temperature Control PID: %s\n", get_value32(Temp_Control) ? "AUTOMATIC":"MANUAL");
            printf("\t[6]  Heater AC: %f\n", get_value64(HeaterAC));
            printf("\t[7]  Heater BD: %f\n", get_value64(HeaterBD));
            printf("\t[8]  Reset: %d\n", get_value32(Reset));
            printf("\t[9]  Reprogramming: %d\n", get_value32(Reprogramming));
            printf("\t[10] New FW Data\n");
            printf("\t[11] Firmware version: %s\n", FW_VERSION);
            printf("\t[12] PID_AC_Kc: %f\n", get_value64(PID_AC_Kc));
            printf("\t[13] PID_AC_tauI: %f\n", get_value64(PID_AC_tauI));
            printf("\t[14] PID_AC_tauD: %f\n", get_value64(PID_AC_tauD));
            printf("\t[15] PID_BD_Kc: %f\n", get_value64(PID_BD_Kc));
            printf("\t[16] PID_BD_tauI: %f\n", get_value64(PID_BD_tauI));
            printf("\t[17] PID_BD_tauD: %f\n", get_value64(PID_BD_tauD));
            printf("\n");
        } else if (strcmp( cmd, "set" ) == 0) {
            if ((arg[0] == NULL) || (arg[1] == NULL)) {
                printf("Command \"set\" used but no arguments given! Type \"help\" to see its correct usage.\n");
                continue;
            }
            uint8_t var_index = strtol( arg[0], NULL, 10);

            if (rffe_vars[var_index].info.writable == READ_ONLY) {
                printf("The requested variable is READ_ONLY!\n");
                continue;
            }
            if (rffe_vars[var_index].info.size == sizeof(int)){
                int arg_int = strtol( arg[1], NULL, 10);
                set_value( (int *)rffe_vars[var_index].data, arg_int);
            } else if ( (rffe_vars[var_index].info.size == sizeof(double)) ) {
                double arg_dbl = strtod( arg[1], NULL);
                set_value( (double *)rffe_vars[var_index].data, arg_dbl);
            } else {
                printf("Unknown data type to set!\n");
            }

        } else if ((strcmp( cmd, "help" ) == 0) || (strcmp( cmd, "?" ) == 0) ) {
            printf("RFFE Firmware help. Available commands:\n");
            printf("\tCMD\t[arg1]\t[arg2]\n");
            printf("\tdump\t\t\tList all variables available and their current status\n");
            printf("\tset\t[VAR]\t[VALUE]\tSet value to a variable in the list\n");
            printf("\thelp\t\t\tShow this help menu\n");
        } else {
            printf("Command \"%s\" not recognized! Please use the command \"help\" to check the CLI usage\n", cli_cmd);
        }
    }
}

int main( void )
{
    //Init serial port for info printf
    pc.baud(115200);

    printf("RFFE Control Firmware %s\n", FW_VERSION);

    bsmp_server_t *bsmp = bsmp_server_new();
    led_g=0;
    led_r=0;

    // Variables initialization
    // Attenuators
    set_value(Att, 30.0);
    // TempAC
    set_value(TempAC, 0.0);
    // TempBD
    set_value(TempBD, 0.0);
    // Set_PointAC
    set_value(Set_PointAC, 51.5);
    // Set_PointBD
    set_value(Set_PointBD, 51.5);
    // Temp_Control
    set_value(Temp_Control, 0);
    // HeaterAC
    set_value(HeaterAC, 0.0);
    // HeaterBD
    set_value(HeaterBD, 0.0);
    // Reset
    set_value(Reset, 0);
    // Reprogramming
    set_value(Reprogramming, 0);
    // Version
    set_value(Version, FW_VERSION, sizeof(FW_VERSION));
    //PID_AC Kc parameter
    set_value(PID_AC_Kc, 10.5);
    //PID_AC tauI parameter
    set_value(PID_AC_tauI, 12);
    //PID_AC tauI parameter
    set_value(PID_AC_tauD, 2);
    //PID_BD Kc parameter
    set_value(PID_BD_Kc, 10.5);
    //PID_BD tauI parameter
    set_value(PID_BD_tauI, 12);
    //PID_BD tauI parameter
    set_value(PID_BD_tauD, 2);

    for ( uint8_t i = 0; i < sizeof(rffe_vars)/sizeof(rffe_vars[0]); i++) {
        rffe_vars[i].info.id = i;
        bsmp_register_variable( bsmp, &rffe_vars[i] );
    }

    int s,numVer;
    uint8_t state = 0;

    struct bsmp_raw_packet request;
    struct bsmp_raw_packet response;
    uint8_t buf[BUFSIZE];
    uint8_t bufresponse[BUFSIZE];
    char path[15], old_name[15], name[15];

    DIR *d = opendir("/local");
    struct dirent *p;
    numVer = 10000;
    while((p = readdir(d)) != NULL) {
        if (check_name(p->d_name) && check_name(p->d_name) < numVer) {
            strcpy(old_name, p->d_name);
            numVer = check_name(p->d_name);
        }
    }
    //printf("old_name: %s\n",old_name);
    closedir(d);

    // *************************************Threads***************************************

    Thread Attenuators_thread(Attenuators_Control);
    printf("Initializing Attenuators thread\n");

    Thread Temp_Control_thread(Temp_Feedback_Control);
    printf("Initializing Temp Control thread\n");

    Thread CLI_Proccess_Thread(CLI_Proccess);

    // Instantiate our command processor for the  USB serial line.
    scMake(&pc, commandCallback, &CLI_Proccess_Thread);

    // Ethernet initialization
    EthernetInterface eth;
    TCPSocketConnection client;
    TCPSocketServer server;
    int recv_sz, sent_sz;

#if defined(ETH_DHCP)
    eth.init(); //Use DHCP
#else
#if defined(ETH_FIXIP)
    eth.init(ETH_IP,ETH_MASK,ETH_GATEWAY); //Use given parameters for static IP
#else
#error "No Ethernet addressing mode selected! Please choose between DHCP or Fixed IP!"
#endif
#endif

    LedY = 0;

    while (true) {
        printf("Trying to bring up ethernet connection... ");
        while (eth.connect(5000) != 0) {
            printf("Attempt failed. Trying again... \n");
        }
        printf("Success! RFFE eth server is up!\n");

        strncpy(rffe_ip, eth.getIPAddress(), sizeof(rffe_ip));
        strncpy(rffe_mac, eth.getMACAddress(), sizeof(rffe_mac));

        printf("RFFE IP: %s\n", rffe_ip);
        printf("RFFE MAC Address: %s\n", rffe_mac);
        printf("Listening on port %d...\n", SERVER_PORT);

        server.bind(SERVER_PORT);
        server.listen();

        /* Turn the conection indicator LED on */
        LedY = 1;

        while (true) {
            printf(" Waiting for new client connection...\n");

            server.accept(client);
            client.set_blocking(1500);

            printf("Connection from client: %s\n", client.get_address());

            while (client.is_connected() && get_eth_link_status()) {

                /* Wait to receive data from client */
                recv_sz = client.receive_all((char*)buf, 3);

                if (recv_sz == 3) {
                    /* We received a complete message header */
                    uint16_t payload_len = (buf[1] << 8) | buf[2];
                    recv_sz += client.receive_all( (char*) &buf[3], payload_len );
                } else {
                    printf( "Received malformed message header of size: %d , discarding...", recv_sz );
                    continue;
                }
                /* Pulse activity LED */
                LedG = 1;


#ifdef DEBUG_PRINTF
                printf("Received message of %d bytes: ", recv_sz);
                for (int i = 0; i < recv_sz; i++) {
                    printf("0x%X ",buf[i]);
                }
                printf("\n");
#endif

                request.data = buf;
                request.len = recv_sz;

                response.data = bufresponse;

                bsmp_process_packet(bsmp, &request, &response);

                sent_sz = client.send_all((char*)response.data, response.len);
#ifdef DEBUG_PRINTF
                printf("Sending message of %d bytes: ", sent_sz);
                for (int i = 0; i < sent_sz; i++) {
                    printf("0x%X ",response.data[i]);
                }
                printf("\n");
#endif

                if (sent_sz <= 0) {
                    printf("ERROR while writing to socket!");
                    LedG = 0;
                    continue;
                }
#if 0
                if (state != Reprogramming[0]) {
                    switch (Reprogramming[0]) {
                    case 0:
                        if (state == 1)
                            fclose(fp);
                        state = Reprogramming[0];
                        break;

                    case 1:
                        DIR *d = opendir("/local");
                        struct dirent *p;
                        numVer = 10000;
                        while((p = readdir(d)) != NULL) {
                            if (check_name(p->d_name) && check_name(p->d_name) < numVer) {
                                strcpy(name, p->d_name);
                                numVer = check_name(p->d_name);
                            }
                        }
                        closedir(d);
                        strcpy(name,old_name);
                        strcpy(path,"/local/");
                        int2str(&name[3],(str2int(&old_name[3])%9999+1));
                        printf("name: %s\n",name);
                        fp = fopen((char*)strcat(path,name), "wb");
                        state = Reprogramming[0];
                        break;

                    case 2:
                        if (state == 1) {
                            fclose(fp);
                        }
                        state = Reprogramming[0];
                        Update_Software(old_name, name);
                        break;

                    }
                }

                if ((Reprogramming[0] == 1) && (buf[0] == 0x20) && (buf[3] == DataID)) {
                    Data_Check();
                }
#endif
                if (get_value32(Reset) == 1) {
                    printf("Resetting MBED!\n");
                    mbed_reset();
                }

                LedG = 0;
            }

            client.close();

            printf("Client Disconnected!\n");
            LedG = 0;

            if (get_eth_link_status() == 0) {
                /* Eth link is down, clean-up server connection */
                server.close();
                eth.disconnect();
                LedY = 0;
                break;
            }
        }
    }
}
