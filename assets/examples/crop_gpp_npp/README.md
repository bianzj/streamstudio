# Crop GPP/NPP example

Open `project.json` in Stream Studio and run the default VoxelEB configuration.

`validated-carbon-summary.json` is a historical reference from before 1.1.2.
Rerun the project after the PAR and illumination-normalization corrections;
the stored rows are neither current-engine validation nor field observations.

- Domain: 20 m x 20 m x 4 m
- Crop: 144 mature maize plants, C4 physiology
- Voxel: 0.5 m
- Meteorology: 48 half-hour nodes
- Outputs: images, radiation process, energy process, GPP, and NPP

GPP is gross leaf photosynthesis. NPP is the current leaf net-assimilation
approximation (GPP minus dark respiration); stem and root respiration are not
yet included.
