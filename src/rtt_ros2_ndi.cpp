#include <tf2/convert.h>
#include <tf2/transform_datatypes.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <boost/asio/serial_port.hpp>
#include <iomanip>
#include <iostream>
#include <regex>
#include <rtt/Component.hpp>
#include <rtt/internal/GlobalService.hpp>
#include <rtt_ros2_ndi/rtt_ros2_ndi.hpp>
#include <rtt_ros2_node/rtt_ros2_node.hpp>
#include <sstream>

rtt_ros2_ndi::rtt_ros2_ndi(const std::string& name)
    : RTT::TaskContext(name),
      serial(io_context),
      configure_sensor("configureSensor", &rtt_ros2_ndi::configureSensor, this,
                       RTT::ClientThread) {
    RTT::Logger::In(this->getName());
    RTT::log(RTT::Info) << "Constructing NDI component!" << RTT::endlog();

    // Get the ROS service and create a node
    ros = RTT::internal::GlobalService::Instance()->getService("ros");
    RTT::OperationCaller<bool()> create_node = ros->getOperation("create_node");
    if (create_node.ready()) {
        create_node();
    } else {
        RTT::log(RTT::Error) << "ROS service not available" << RTT::endlog();
    }

    // get the TF service and the broadcast operation
    tf2 = ros->getService("tf2");
    if (tf2) {
        if (!broadcastTransform.setImplementationPart(
                tf2->getOperation("broadcastTransform"))) {
            RTT::log(RTT::Error)
                << "broadcastTransform not available" << RTT::endlog();
        }
    } else {
        RTT::log(RTT::Error) << "tf2 service not loaded" << std::endl;
    }

    addOperation(configure_sensor).doc("Configure rtt_ros2_ndi sensor.");
    addProperty("device_file", device_file);
    addProperty("reference_frame", reference_frame);
}

rtt_ros2_ndi::~rtt_ros2_ndi() {}

bool rtt_ros2_ndi::configureSensor(const std::string& port_handle,
                                   const std::string& name) {
    std::map<std::string, Port>::iterator it = ports.find(port_handle);
    if (it != ports.end()) {
        it->second.name = name;
        RTT::log(RTT::Info) << "Sensor " << port_handle
                            << "alreay configured. Changing its name to "
                            << name << "." << RTT::endlog();
    } else {
        Port port;
        port.name = name;
        ports.insert(std::make_pair(port_handle, port));
    }
    return true;
}

bool rtt_ros2_ndi::configureHook() {
    RTT::log().setLogLevel(RTT::Logger::Error);
    RTT::log(RTT::Info) << "Opening serial port" << RTT::endlog();
    serial.close();
    try {
        serial.open(device_file);
    } catch (boost::system::system_error& e) {
        RTT::log(RTT::Error)
            << "Failed to open serial port: " << device_file << RTT::endlog();
        return false;
    }

    RTT::log(RTT::Info) << "Configuring serial port" << RTT::endlog();
    boost::asio::serial_port::baud_rate BAUD(9600);
    boost::asio::serial_port::character_size csize(8);
    boost::asio::serial_port::parity PARITY(
        boost::asio::serial_port::parity::none);
    boost::asio::serial_port::stop_bits STOP(
        boost::asio::serial_port::stop_bits::one);
    boost::asio::serial_port::flow_control FLOW(
        boost::asio::serial_port::flow_control::none);

    try {
        serial.set_option(BAUD);
        serial.set_option(csize);
        serial.set_option(PARITY);
        serial.set_option(STOP);
        serial.set_option(FLOW);
    } catch (boost::system::system_error& e) {
        RTT::log(RTT::Error)
            << "Failed to configure serial port." << RTT::endlog();
        return false;
    }

    unsigned int N = 0;

    serial.send_break();
    
    RTT::log(RTT::Info) << "Reset" << RTT::endlog();
    send("RESET 1");
    if (recv("RESETBE6F") == EFAILURE) {
        std::cout << buffer << std::endl;
        RTT::log(RTT::Error) << "Failed to reset." << RTT::endlog();
        return false;
    }
    
    RTT::log(RTT::Info) << "Set Baud rate" << RTT::endlog();
    send("COMM A0001");
    if (recv("OKAY") == EFAILURE) {
        std::cout << buffer << std::endl;
        RTT::log(RTT::Error) << "Failed to set the Baud rate." << RTT::endlog();
        return false;
    }

    // reopen the serial port at 230400 + flow control
    try {
      serial.set_option(boost::asio::serial_port::baud_rate(230400));
      serial.set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::hardware));
    } catch (boost::system::system_error& e) {
      RTT::log(RTT::Error)
	<< "Failed to configure serial port." << RTT::endlog();
      return false;
    }
    
    RTT::log(RTT::Info) << "Initializing NDI." << RTT::endlog();
    send("INIT ");
    if (recv("OKAY") == EFAILURE) {
        std::cout << buffer << std::endl;
        RTT::log(RTT::Error) << "Failed to initialize NDI." << RTT::endlog();
        return false;
    }

    RTT::log(RTT::Info) << "Beeping NDI." << RTT::endlog();
    send("BEEP 2");

    // ports to be freed
    {
        RTT::log(RTT::Info) << "Port Handle PHSR 01." << RTT::endlog();
        send("PHSR 01");
        recv();

        char phsr01[BUF_SIZE];
        memcpy(phsr01, buffer, BUF_SIZE);
        sscanf(phsr01, "%02X", &N);
        RTT::log(RTT::Info)
            << "Number of port to be freed: " << N << RTT::endlog();

        char* ptr = phsr01 + 2;
        for (int i = 0; i < N; i++) {
            char str[3];
            sscanf(ptr, "%2c%*3c", str);
            ptr += 5;
            str[2] = '\0';
            send("PHF " + std::string(str));
            recv();
        }
    }

    // ports to be initialied
    {
        RTT::log(RTT::Info) << "Port Handle PHSR 02." << RTT::endlog();
        send("PHSR 02");
        recv();

        char phsr02[BUF_SIZE];
        memcpy(phsr02, buffer, BUF_SIZE);
        sscanf(phsr02, "%02X", &N);
        RTT::log(RTT::Info)
            << "Number of allocated port initialized: " << N << RTT::endlog();

        char* ptr = phsr02 + 2;
        for (unsigned int i = 0; i < N; i++) {
            char str[3];
            sscanf(ptr, "%2c%*3c", str);
            ptr += 5;
            str[2] = '\0';
            ports.insert(std::make_pair(std::string(str), Port()));
            send("PINIT " + std::string(str));
            recv();
        }
    }

    //
    {
        RTT::log(RTT::Info) << "Port Handle PHSR 00." << RTT::endlog();
        send("PHSR 00");
        recv();

        char phsr00[BUF_SIZE];
        memcpy(phsr00, buffer, BUF_SIZE);
        sscanf(phsr00, "%02X", &N);
        RTT::log(RTT::Info)
            << "Number of allocated port handles: " << N << RTT::endlog();

        char* ptr = phsr00 + 2;
        for (unsigned int i = 0; i < N; i++) {
            char str[3];
            sscanf(ptr, "%2c%*3c", str);
            ptr += 5;
            str[2] = '\0';
            send("PHINF " + std::string(str) + "0005");
            std::cout << "PHINF " << std::string(str) << "0005" << std::endl;
            recv();

            char type[3], sernum[9];
            sscanf(buffer, "%2c%*1X%*1X%*2c%*2c%*12c%*3c%8c%*2c%*20c", type,
                   sernum);
            type[2] = '\0';
            sernum[8] = '\0';
            std::map<std::string, Port>::iterator it =
                ports.find(std::string(str));
            if (it != ports.end()) {
                it->second.serial_number = std::string(sernum);
                std::cout << "sernum " << std::string(sernum) << std::endl;
                it->second.type = std::string(type);
                std::cout << it->second << std::endl;
            }
        }
    }

    {
        RTT::log(RTT::Info) << "Port Handle PHSR 03." << RTT::endlog();
        send("PHSR 03");
        recv();

        char phsr03[BUF_SIZE];
        memcpy(phsr03, buffer, BUF_SIZE);
        sscanf(phsr03, "%02X", &N);

        char* ptr = phsr03 + 2;
        for (unsigned int i = 0; i < N; i++) {
            char str[3];
            sscanf(ptr, "%2c%*3c", str);
            ptr += 5;
            str[2] = '\0';
            send("PENA " + std::string(str) + "D");
            recv();
        }
    }

    return true;
}

bool rtt_ros2_ndi::startHook() {
    RTT::log(RTT::Info) << "Starting NDI thread." << RTT::endlog();
    send("TSTART 80");
    return recv("OKAY") == ESUCCESS;
}

void rtt_ros2_ndi::updateHook() {
    send("TX 0001");
    recv();

    char tx0001[BUF_SIZE];
    memcpy(tx0001, buffer, BUF_SIZE);
    unsigned int N = 0;
    sscanf(tx0001, "%02X", &N);
    char* ptr = tx0001 + 2;

    std::vector<geometry_msgs::msg::TransformStamped> transforms;
    int ref = -1, tip = -1;
    for (unsigned int i = 0; i < N; i++) {
        char ph[3];
        sscanf(ptr, "%2c", ph);
        ph[2] = '\0';
        ptr += 2;

        if (strncmp(ptr, "MISSING", 7) == 0) {
            RTT::log(RTT::Info)
                << "Port handle " << ph << " missing." << RTT::endlog();
        } else if (strncmp(ptr, "DISABLED", 8) == 0) {
            RTT::log(RTT::Info)
                << "Port handle " << ph << " disabled." << RTT::endlog();
        } else if (strncmp(ptr, "UNOCCUPIED", 10) == 0) {
            RTT::log(RTT::Info)
                << "Port handle " << ph << " unoccupied." << RTT::endlog();
        } else {
            double q[4], p[3], error;
            sscanf(ptr, "%6lf%6lf%6lf%6lf%7lf%7lf%7lf%6lf%*8X", &q[3], &q[0],
                   &q[1], &q[2], &p[0], &p[1], &p[2], &error);
            ptr += ((4 * 6) + (3 * 7) + 6 + 8);

            for (int i = 0; i < 3; i++) p[i] = p[i] / (100.0 * 1000.0);
            for (int i = 0; i < 4; i++) q[i] = q[i] / 10000.0;
            /*
              std::cout << ph << ": "
              << std::setw(13) << q[0]
              << std::setw(13) << q[1]
              << std::setw(13) << q[2]
              << std::setw(13) << q[3]
              << std::setw(13) << p[0]
              << std::setw(13) << p[1]
              << std::setw(13) << p[2]
              //<< std::setw(13) << error/10000.0
              << std::endl;
              //std::cout << q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3] <<
              std::endl;
              */

            std::map<std::string, Port>::iterator it =
                ports.find(std::string(ph));
            if (it != ports.end()) {
                geometry_msgs::msg::TransformStamped transform;
                transform.header.frame_id = "NDI";
                transform.child_frame_id = it->second.name;
                if (it->second.name == reference_frame) {
                    ref = i;
                } else if (it->second.name == "tip") {
                    tip = i;
                }
                transform.transform.translation.x = p[0];
                transform.transform.translation.y = p[1];
                transform.transform.translation.z = p[2];
                transform.transform.rotation.x = q[0];
                transform.transform.rotation.y = q[1];
                transform.transform.rotation.z = q[2];
                transform.transform.rotation.w = q[3];
                // broadcastTransform( transform );
                transforms.push_back(transform);
            } else {
                RTT::log(RTT::Error)
                    << "Handle not found " << ph << RTT::endlog();
            }
        }
        unsigned int frame;
        sscanf(ptr, "%08X", &frame);
        ptr += 8;
        ptr++;
    }
    if (ref == -1) {
        RTT::log(RTT::Error) << "Reference frame " << reference_frame
                             << " was not found." << RTT::endlog();
    } else if (tip == -1) {
        RTT::log(RTT::Error) << "Tip not found " << RTT::endlog();
    } else {
        tf2::Transform Rt_NDI_ref;
        tf2::fromMsg(transforms[ref].transform, Rt_NDI_ref);

        tf2::Transform Rt_offset(tf2::Quaternion(1.0, 0.0, 0.0, 0.0),
                                 tf2::Vector3{});
        tf2::Transform Rt_ref_NDI = (Rt_NDI_ref * Rt_offset).inverse();

        for (int i = 0; i < N; i++) {
            if (i != ref) {
                tf2::Transform Rt_NDI_i, Rt_ref_i;
                tf2::fromMsg(transforms[i].transform, Rt_NDI_i);

                if (i != tip) Rt_NDI_i = Rt_NDI_i * Rt_offset;

                Rt_ref_i = Rt_ref_NDI * Rt_NDI_i;

                geometry_msgs::msg::TransformStamped transform;
                transform.header.frame_id = transforms[ref].child_frame_id;
                transform.child_frame_id = transforms[i].child_frame_id;
                tf2::convert(Rt_ref_i, transform.transform);
                transform.header.stamp = rtt_ros2_node::getNode(this)->now();

                broadcastTransform(transform);
            }
        }
    }

    ptr += 4;
}

void rtt_ros2_ndi::stopHook() {
    send("TSTOP ");
    recv("OKAYA896");
}

void rtt_ros2_ndi::cleanupHook() {
    RTT::log(RTT::Info) << "Closing serial port" << RTT::endlog();
    serial.close();
}

void rtt_ros2_ndi::send(const std::string& command) {
    std::stringstream buf;
    buf << command << "\r";
    std::string str = buf.str();
    try {
        serial.write_some(boost::asio::buffer(str.data(), str.size()));
    } catch (boost::system::system_error& e) {
        RTT::log(RTT::Error)
            << "Failed to write: " << str.data() << RTT::endlog();
    }
}

rtt_ros2_ndi::Errno rtt_ros2_ndi::recv(const std::string& response) {
    memset(buffer, 0, sizeof(buffer));
    try {
        std::size_t N = 0;
        do {
            N += serial.read_some(
                boost::asio::buffer(buffer + N, sizeof(buffer) - N));
        } while (buffer[N - 1] != '\r');
        buffer[N] = '\0';
    } catch (boost::system::system_error& e) {
        RTT::log(RTT::Error) << "Failed to receive" << RTT::endlog();
    }
    std::string s(buffer);
    if (s.find(response) == std::string::npos) return EFAILURE;
    return ESUCCESS;
}

ORO_CREATE_COMPONENT(rtt_ros2_ndi)
