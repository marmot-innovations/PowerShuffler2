## PowerShuffler 2.0 PCB design schematic, layout, Gerbers, and BOM  
KiCAD 5.0.0 for PCB design  
Microsoft Excel web for BOM  
No 3D model  
No XY pick and place Centroid  
No ODB++  
No footprint library  
No schematic library

Design bugs and comments:  
1. To program master MCU, remove R6 and C7 first, then replace them once programming is completed. These components will put too much load on ISP TPI programmer.
2. To program client MCU, remove C24 and R22 first, then replace them once completed. These components will put too much load on ISP TPI programmer.
3. J2 and J4 do not need soldered headers for programming. Just shove a 5-pin 0.1" straight strip header into it to establish electrical contact. Look up "sneaky footprints PCB" on the web.
4. Mounting holes are for 4-40 screws and stand-offs.
5. Peak 82% efficiency during max charge rate as measured.
6. Less than 0.5mA current draw from input when idle, not charging.
7. There is no D2 LED, only D1 and D3.
