% 2DX3 Final Project - MATLAB 3D Visualization
% Subhan Razzaq, 400557958, razzas2
%
% Reads scan data from the microcontroller over UART
% Data format from MCU:
%   BEGIN_DATA
%   SCAN <index> <x_displacement_mm>
%   <32 distance values, one per line>
%   SCAN ...
%   ...
%   END_DATA

clear; clc; close all;

% serial config
PORT = "COM3";
BAUD_RATE = 115200;

% open serial port
s = serialport(PORT, BAUD_RATE);
configureTerminator(s, "LF");
s.Timeout = 60;    % give it plenty of time since user controls when data is sent

disp("Connected to " + PORT);
disp("Waiting for data from MCU... (press PJ0 on the board to send)");

% read lines until we see BEGIN_DATA
while true
    line = strtrim(readline(s));
    fprintf("RX: %s\n", line);
    if strcmp(line, "BEGIN_DATA")
        break;
    end
end

disp("Receiving scan data...");

% parse all the scan data
allPoints = [];          % will hold [x, y, z] for every point
scanCount = 0;
currentX = 0;            % x displacement for current scan

while true
    line = strtrim(readline(s));
    
    % done receiving
    if strcmp(line, "END_DATA")
        break;
    end
    
    % check if this is a scan header line
    tokens = strsplit(line);
    if strcmp(tokens{1}, "SCAN")
        scanCount = scanCount + 1;
        currentX = str2double(tokens{3});    % x displacement in mm
        angleIdx = 0;                        % reset angle counter for this scan
        fprintf("  Receiving scan %d (x = %d mm)\n", scanCount, currentX);
        continue;
    end
    
    % otherwise its a distance value
    dist = str2double(line);
    if isnan(dist)
        continue;    % skip any weird lines
    end
    
    angleIdx = angleIdx + 1;
    angle = angleIdx * 11.25;    % each measurement is 11.25 degrees apart
    
    % convert polar (angle, distance) to cartesian (y, z)
    % x comes from the scan displacement
    y = dist * cosd(angle);
    z = dist * sind(angle);
    
    allPoints = [allPoints; currentX, y, z, scanCount]; %#ok<AGROW>
end

% close the port
clear s;
fprintf("\nReceived %d scans, %d total points.\n", scanCount, size(allPoints, 1));

% ---- 3D Plot ----
if ~isempty(allPoints)
    figure('Name', '2DX3 Spatial Map', 'NumberTitle', 'off');
    
    colors = lines(scanCount);   % different color per scan
    
    hold on;
    for c = 1:scanCount
        idx = allPoints(:,4) == c;
        pts = allPoints(idx, 1:3);
        % close the loop by appending the first point at the end
        pts = [pts; pts(1,:)];
        plot3(pts(:,1), pts(:,2), pts(:,3), 'o-', ...
            'Color', colors(c,:), ...
            'MarkerFaceColor', colors(c,:), ...
            'LineWidth', 1.2, ...
            'DisplayName', sprintf('Scan %d', c));
    end
    hold off;
    
    grid on;
    xlabel('X (mm) - Displacement');
    ylabel('Y (mm)');
    zlabel('Z (mm)');
    title('2DX3 Spatial Map - Subhan Razzaq');
    legend('show', 'Location', 'best');
    view(3);
    rotate3d on;
    
    % equalize Y and Z axes only so scans look circular, leave X free
    yl = ylim;
    zl = zlim;
    maxRange = max(abs([yl, zl]));
    ylim([-maxRange maxRange]);
    zlim([-maxRange maxRange]);
else
    disp("No data received.");
end