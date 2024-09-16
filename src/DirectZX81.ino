/* DirectZX81 is 8-bit computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * Cassette recorder is emulated using SPIFFS. The user can save his programs 
 * in SPIFFS in the form of files with the extension .Z81. Extension .Z81 is  
 * added automatically. Command example: SAVE "PROGRAM"
 * 
 * DirectZX81 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * DirectZX81 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2024
 * Version 1.0
*/
#include "fabgl.h"
#include "emudevs/Z80.h"          // For processor
#include "SPIFFS.h"

fabgl::VGADirectController DisplayController;
fabgl::PS2Controller PS2Controller;

// Constants for video output
static constexpr int borderSize           = 54;
static constexpr int borderXSize          = 72;
static constexpr int scanlinesPerCallback = 2;  // screen height should be divisible by this value

static TaskHandle_t  mainTaskHandle;
void * auxPoint;  // For use in setup

// **************************************************************************************************
// Hardware emulated in the ZX 81 computer
// "Tape recorder" is emulated in SPIFFS -> .Z81 file with NAME.Z81, started up with command LOAD "NAME"
File file;
bool isTape = false;      // Is SPIFFS ready
String fileName;          // Name of saved/opened file

// We will use processor Z80 from the library FabGL
fabgl::Z80 m_Z80;
// Variables for emulating keyboard connection
int keyboardIn[8];      // Value read from keyboard port on individual lines
// Variables for VGA and colors
uint8_t darkbgcolor, darkcolor, whitecolor;        // Colors - must be filled in setup()
int width, height;          // Display size - must be filled in setup()

// Memory will be just Byte array
uint8_t ZXram[32768];   // selected addresses are overwritten by ROM in read mode
// ROM memory is contained in the array "zx81rom[]"
#include "zx81rom.h"
// Auxiliary global variables
unsigned int actualPC;  // Actual PC value is needed for IN operation
uint16_t lineDispPoint = 16509;   // Pointer to RAM, where is row for display

// **************************************************************************************************
// Functions for communication on the bus
static int readByte(void * context, int address)              { return(ZXram[address & 0x7FFF]); };
static void writeByte(void * context, int address, int value) { if(address > 0x1FFF) ZXram[address & 0x7FFF] = (uint8_t)value; };   // Protect ROM area
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); };
static int readIO(void * context, int address)
{
  // *** Keyboard inputs ULA has only 1 address
  if(address == 0xFE) {
    // This is work around of Lin Ke-Fong emulator imperfection (I/O in emulator has only byte address)
    // according to the instruction IN A,(n) ** 0xDB ** or IN r,(C) ** 0xED ** is content of register A or B simulated on higher address bus lines
    uint8_t adrKey, keyOut = 0x3F; // High byte of address and output value I/O read 
    if(ZXram[actualPC] == 0xDB) adrKey = m_Z80.readRegByte(Z80_A);
      else adrKey = m_Z80.readRegByte(Z80_B);
    // Some games test more keys at one IN instruction, so put together keyboard status
    if(!(adrKey & 0x01)) keyOut &= keyboardIn[0];
    if(!(adrKey & 0x02)) keyOut &= keyboardIn[1];
    if(!(adrKey & 0x04)) keyOut &= keyboardIn[2];
    if(!(adrKey & 0x08)) keyOut &= keyboardIn[3];
    if(!(adrKey & 0x10)) keyOut &= keyboardIn[4];
    if(!(adrKey & 0x20)) keyOut &= keyboardIn[5];
    if(!(adrKey & 0x40)) keyOut &= keyboardIn[6];
    if(!(adrKey & 0x80)) keyOut &= keyboardIn[7];
    return keyOut;
  } else return 0x20; 
};
static void writeIO(void * context, int address, int value)
{
  // No output function
};

// **************************************************************************************************
// Keyboard interface for selected keys
// Handles Key Up following keys:
void procesKeyUp(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] |= 0x01; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[3] |= 0x01; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] |= 0x02; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[3] |= 0x04; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] |= 0x08; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[3] |= 0x10; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[4] |= 0x10; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[4] |= 0x08; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[4] |= 0x04; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[4] |= 0x02; break;  // 9

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[2] |= 0x01; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[2] |= 0x02; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] |= 0x04; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[2] |= 0x08; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[2] |= 0x10; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[5] |= 0x10; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[5] |= 0x08; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[5] |= 0x04; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[5] |= 0x02; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[5] |= 0x01; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[1] |= 0x01; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] |= 0x02; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[1] |= 0x04; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] |= 0x08; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[1] |= 0x10; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[6] |= 0x10; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] |= 0x08; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[6] |= 0x04; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[6] |= 0x02; break;  // l-L

      case VirtualKey::VK_SPACE: keyboardIn[7] |= 0x01; break;  // space
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[0] |= 0x02; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[0] |= 0x04; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[0] |= 0x08; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] |= 0x10; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[7] |= 0x10; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[7] |= 0x08; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] |= 0x04; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardIn[7] |= 0x02; break; break;  // Ctrl
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[6] |= 0x01; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[0] |= 0x01;  // L and R shift
      default: break;
      }
};

// Handles Key Down following keys:
void procesKeyDown(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_ESCAPE: m_Z80.reset(); break;  // ESC will reset computer

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] &= 0xFE; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[3] &= 0xFE; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] &= 0xFD; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[3] &= 0xFB; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] &= 0xF7; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[3] &= 0xEF; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[4] &= 0xEF; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[4] &= 0xF7; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[4] &= 0xFB; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[4] &= 0xFD; break;  // 9

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[2] &= 0xFE; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[2] &= 0xFD; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] &= 0xFB; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[2] &= 0xF7; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[2] &= 0xEF; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[5] &= 0xEF; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[5] &= 0xF7; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[5] &= 0xFB; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[5] &= 0xFD; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[5] &= 0xFE; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[1] &= 0xFE; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] &= 0xFD; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[1] &= 0xFB; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] &= 0xF7; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[1] &= 0xEF; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[6] &= 0xEF; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] &= 0xF7; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[6] &= 0xFB; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[6] &= 0xFD; break;  // l-L

      case VirtualKey::VK_SPACE: keyboardIn[7] &= 0xFE; break;  // space
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[0] &= 0xFD; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[0] &= 0xFB; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[0] &= 0xF7; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] &= 0xEF; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[7] &= 0xEF; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[7] &= 0xF7; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] &= 0xFB; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardIn[7] &= 0xFD; break;  // Ctrl
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[6] &= 0xFE; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[0] &= 0xFE; break;  // L and R shift
      default: break;
      }
};

// **************************************************************************************************
// VGA main function - prepare lines for displaying
void IRAM_ATTR drawScanline(void * arg, uint8_t * dest, int scanLine)
{
  // draws "scanlinesPerCallback" scanlines every time drawScanline() is called
  for (int i = 0; i < scanlinesPerCallback; ++i) {
    // fill border with background color
    memset(dest, darkbgcolor, width);
    if (!((scanLine < borderSize) || (scanLine >= (192+borderSize)))) {   // ZX81 display is 192 rows height
      memset(dest+borderXSize, whitecolor, 256); //for (int i = 0; i < 256; i++) VGA_PIXELINROW(dest, i+borderXSize) = whitecolor;
      uint16_t lineInChar = (scanLine - borderSize) % 8;      // What line in 8*8 character is displayed
      uint16_t charInRow = lineDispPoint + 1;      // Row starts with 0x76 code that is not displayed
      for (int k = 0; k < 32; k++)  // 32 bytes must be transformed to 256 pixels on row
        {
          uint8_t videobyte = ZXram[charInRow];  // Row Video RAM start address
          if(videobyte == 0x76) videobyte == 0x00; else charInRow++;    // Char 0x76 will stop displaying row and "space" will continue
          uint8_t charline = ZXram[0x1E00 + ((((int)videobyte)*8)&0x1FF) + lineInChar];  // Line of character from ROM table will be displayed
          if (videobyte & 0x80) {   // Normal or reverse colors
            uint8_t shiftr = 0x80;
            for(int j=0; j<8; j++) {      // Set pixels on screen according this byte 
              if(!(charline & shiftr)) VGA_PIXELINROW(dest, k*8+j+borderXSize) = darkcolor; // Reverse
              shiftr = shiftr >>1;
              }
          } else {
            uint8_t shiftr = 0x80;
            for(int j=0; j<8; j++) {
              if(charline & shiftr) VGA_PIXELINROW(dest, k*8+j+borderXSize) = darkcolor; // Normal
              shiftr = shiftr >>1;
              }
           }
        }
      if(lineInChar == 7) lineDispPoint = charInRow;      // Shift pointer to next text row
    }
    // go to next scanline
    ++scanLine;
    dest += width;
  }
  if (scanLine == height) {
    // signal end of screen
    vTaskNotifyGiveFromISR(mainTaskHandle, NULL);
  }
}


// **************************************************************************************************
void setup()
{
  mainTaskHandle = xTaskGetCurrentTaskHandle();

  // Start SPIFFS emulating tape recorder
  if(SPIFFS.begin(true)) isTape = true;

  // Copy ROM to RAM memory, where ROM area will be protected
  for(int i=0; i<8192; i++) ZXram[i] = zx81rom[i];

  // Set VGA for display monitor
  DisplayController.begin();
  DisplayController.setScanlinesPerCallBack(scanlinesPerCallback);
  DisplayController.setDrawScanlineCallback(drawScanline);
  DisplayController.setResolution(VGA_400x300_60Hz);
  width  = DisplayController.getScreenWidth();
  height = DisplayController.getScreenHeight();
  // Creating table of colors for VGA - table is 128 byte long, but only special positions are needed
  darkbgcolor = DisplayController.createRawPixel(RGB222(1, 1, 1)); // grey for background
  darkcolor = DisplayController.createRawPixel(RGB222(0, 0, 0)); // black
  whitecolor = DisplayController.createRawPixel(RGB222(2, 2, 2)); // white

  // Set CPU bus functions and start it
  m_Z80.setCallbacks(auxPoint, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
  m_Z80.reset();
  for (int i = 0; i < 8; i++) keyboardIn[i]=0xFF;

  // Set function pro Keyboard processing
  PS2Controller.begin(PS2Preset::KeyboardPort0, KbdMode::GenerateVirtualKeys);
  PS2Controller.keyboard()->onVirtualKey = [&](VirtualKey * vk, bool keyDown) {
      if (keyDown) {
        procesKeyDown(*vk);
    } else procesKeyUp(*vk);
  };
}

// Gets file name from ZX81 RAM (global "fileName" is filled)
bool getFileName()
{
  char convTable[] = {"____________________________0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"};      // ZX81 does not use ASCII, so convert it
  fileName = "/";   // File in root
  unsigned int namePoint = (unsigned int)ZXram[16404] + ((unsigned int)ZXram[16405] << 8);     // Command LOAD/SAVE starts here
  // Try to find begin of name (omit spaces and so) - get first letter
  for(int i=0; i<10; i++) if((ZXram[namePoint] < 0x26) || ((ZXram[namePoint] & 0x7F) > 0x3F)) namePoint++;  // Try it max 10times
  if((ZXram[namePoint] < 0x26) || (ZXram[namePoint] > 0x3F)) return false;  // Name must start with letter
  unsigned int k=0;  // counter for parsing file name in ZX81 memory
  do{
    fileName += convTable[(ZXram[namePoint+k] & 0x7F)]; k++;      // Parse file name
  } while((ZXram[namePoint+k] > 0x1B) && ((ZXram[namePoint+k] & 0x7F) < 0x40) && (k<10));    // 10 characters max and special char is stop
  fileName+=".Z81";
  return true;
}

// Save program to file .Z81
bool saveFile()
{
  if(!getFileName()) return false;
  int fileLenght = ((int)ZXram[0x4014] & 0x0FF) + (((int)ZXram[0x4015] << 8) & 0xFF00) - 0x4008;
  uint8_t *pointStart = ZXram + 0x4009;
  if(isTape) {
    File file = SPIFFS.open(fileName, "wb");
    if(file){ // Error will exit file save
      if(file.write(pointStart, fileLenght)) {
        file.close();
        return true;  // Success in save function
      } else { file.close(); return false; }
    } else return false;
  } return false;
}
// Load program from file .Z81
bool loadFile()
{
  if(!getFileName()) return false;
  if(isTape) {
    // If SPIFFS is ready, try open file
    File file = SPIFFS.open(fileName, "rb");
    if(file){ // Error will exit file load
      size_t fileSize = file.size();    // Get size of file
      if(fileSize < 16374) {            // Test if not crossed the end of the memory
        fileSize = file.readBytes((char*)(ZXram + 0x4009), file.size());   // Read file to memory
        file.close();
        if(fileSize>0) return true;  else return false;   // Success or error in load function
      } else { file.close(); return false;}
    } else return false;
  } return false;
}

// **************************************************************************************************
// **************************************************************************************************

void loop()
{
  static int numCycles;
  numCycles = 0;
  while(numCycles < 25000) {  // approx. 25000 cycles per 16.6 milisec (60 Hz VGA)
    numCycles += m_Z80.step();
    actualPC = m_Z80.getPC() & 0x7FFF;     // Get PC value for input operation on the bus (see readIO() )
    if(m_Z80.getPC() == 0x02F9) {      // Instead of SAVE procedure in ROM, the memory will save to the .Z81 file
      if(saveFile()) m_Z80.setPC(0x03A6); else m_Z80.setPC(0x02F4);  // Report D - OK or F - failure is displayed
    }
    if(m_Z80.getPC() == 0x0343) {      // Instead of the LOAD procedure in ROM, the filling of the memory with the .Z81 file is started
      if(loadFile()) m_Z80.setPC(0x03A6); else m_Z80.setPC(0x02F4);  // Report D - OK or F - failure is displayed
    }
  }
  lineDispPoint = (uint16_t)ZXram[16396] + ((uint16_t)ZXram[16397] << 8);   // Again start pointer to display

  // wait for vertical sync
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
