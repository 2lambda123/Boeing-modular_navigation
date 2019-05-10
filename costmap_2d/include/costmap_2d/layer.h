#ifndef COSTMAP_2D_LAYER_H_
#define COSTMAP_2D_LAYER_H_

#include <costmap_2d/costmap_2d.h>
#include <costmap_2d/layered_costmap.h>
#include <string>
#include <tf2_ros/buffer.h>

namespace costmap_2d
{
class LayeredCostmap;

class Layer
{
  public:
    Layer();

    void initialize(LayeredCostmap* parent, std::string name, tf2_ros::Buffer* tf);

    /**
     * @brief This is called by the LayeredCostmap to poll this plugin as to how
     *        much of the costmap it needs to update. Each layer can increase
     *        the size of this bounds.
     *
     * For more details, see "Layered Costmaps for Context-Sensitive Navigation",
     * by Lu et. Al, IROS 2014.
     */
    virtual void updateBounds(const double robot_x, const double robot_y, const double robot_yaw, double* min_x,
                              double* min_y, double* max_x, double* max_y) = 0;

    /**
     * @brief Actually update the underlying costmap, only within the bounds calculated during UpdateBounds().
     */
    virtual void updateCosts(Costmap2D& master_grid, unsigned int min_i, unsigned int min_j, unsigned int max_i,
                             unsigned int max_j) = 0;

    /** @brief Stop publishers. */
    virtual void deactivate() = 0;

    /** @brief Restart publishers if they've been stopped. */
    virtual void activate() = 0;

    virtual void reset() = 0;

    virtual ~Layer(){};

    /**
     * @brief Check to make sure all the data in the layer is up to date.
     *        If the layer is not up to date, then it may be unsafe to
     *        plan using the data from this layer, and the planner may
     *        need to know.
     *
     *        A layer's current state should be managed by the protected
     *        variable current_.
     * @return Whether the data in the layer is up to date.
     */
    bool isCurrent() const
    {
        return current_;
    }

    /** @brief Implement this to make this layer match the size of the parent costmap. */
    virtual void matchSize() = 0;

    std::string getName() const
    {
        return name_;
    }

  protected:
    virtual void onInitialize() = 0;

    LayeredCostmap* layered_costmap_;
    bool current_;
    bool enabled_;

    std::string name_;
    tf2_ros::Buffer* tf_;
};
}

#endif
